/* Linux io_uring based ae.c module using liburing
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <liburing.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>

/* liburing provides all the necessary structures and functions */

/* Async request tracking */
typedef struct uring_async_req {
    int type;                           /* AE_ASYNC_READ, AE_ASYNC_WRITE, AE_ASYNC_ACCEPT */
    int fd;
    void *buf;
    size_t len;
    union {
        aeAsyncReadProc *read_cb;
        aeAsyncWriteProc *write_cb;
        aeAsyncAcceptProc *accept_cb;
    } callback;
    void *user_data;
    struct uring_async_req *next;
} uring_async_req;

typedef struct aeApiState {
    struct io_uring ring;               /* liburing ring structure */
    struct io_uring_cqe *events;       /* Completion events array */
    int *fd_to_mask;                    /* Map fd to event mask for polling */
    uring_async_req *async_requests;    /* Pending async requests */
} aeApiState;

/* Initialize io_uring using liburing */
static int setup_io_uring(struct io_uring *ring, unsigned entries) {
    return io_uring_queue_init(entries, ring, 0);
}

/* Cleanup io_uring using liburing */
static void cleanup_io_uring(struct io_uring *ring) {
    io_uring_queue_exit(ring);
}

/* Submit a poll operation to io_uring using liburing */
static int submit_poll(struct io_uring *ring, int fd, int mask, void *user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        return -1;
    }

    /* Convert AE mask to poll events */
    __u32 poll_events = 0;
    if (mask & AE_READABLE) poll_events |= POLLIN;
    if (mask & AE_WRITABLE) poll_events |= POLLOUT;

    io_uring_prep_poll_add(sqe, fd, poll_events);
    io_uring_sqe_set_data(sqe, user_data);

    return 0;
}

/* Remove a poll operation from io_uring using liburing */
static int submit_poll_remove(struct io_uring *ring, void *user_data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        return -1;
    }

    io_uring_prep_poll_remove(sqe, user_data);

    return 0;
}

static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;

    /* Initialize io_uring with reasonable queue depth */
    if (setup_io_uring(&state->ring, 256) < 0) {
        zfree(state);
        return -1;
    }

    /* Allocate events array for completion events */
    state->events = zmalloc(sizeof(struct io_uring_cqe) * eventLoop->setsize);
    if (!state->events) {
        cleanup_io_uring(&state->ring);
        zfree(state);
        return -1;
    }

    /* Allocate fd to mask mapping */
    state->fd_to_mask = zmalloc(sizeof(int) * eventLoop->setsize);
    if (!state->fd_to_mask) {
        zfree(state->events);
        cleanup_io_uring(&state->ring);
        zfree(state);
        return -1;
    }

    /* Initialize fd mapping */
    for (int i = 0; i < eventLoop->setsize; i++) {
        state->fd_to_mask[i] = AE_NONE;
    }

    state->async_requests = NULL;
    anetCloexec(state->ring.ring_fd);
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    
    state->events = zrealloc(state->events, sizeof(struct io_uring_cqe) * setsize);
    state->fd_to_mask = zrealloc(state->fd_to_mask, sizeof(int) * setsize);
    
    /* Initialize new fd mappings */
    for (int i = eventLoop->setsize; i < setsize; i++) {
        state->fd_to_mask[i] = AE_NONE;
    }
    
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;
    
    /* Free async requests */
    uring_async_req *req = state->async_requests;
    while (req) {
        uring_async_req *next = req->next;
        zfree(req);
        req = next;
    }
    
    cleanup_io_uring(&state->ring);
    zfree(state->events);
    zfree(state->fd_to_mask);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;

    if (fd >= eventLoop->setsize) return -1;

    int old_mask = state->fd_to_mask[fd];
    int new_mask = old_mask | mask;

    /* If this fd is already being monitored, remove the old poll first */
    if (old_mask != AE_NONE) {
        submit_poll_remove(&state->ring, (void*)(long)fd);
        /* Submit the remove operation */
        if (io_uring_submit(&state->ring) < 0) {
            return -1;
        }
    }

    /* Add new poll operation */
    if (submit_poll(&state->ring, fd, new_mask, (void*)(long)fd) < 0) {
        return -1;
    }

    state->fd_to_mask[fd] = new_mask;

    /* Submit the add operation */
    if (io_uring_submit(&state->ring) < 0) {
        return -1;
    }

    return 0;
}

static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;

    if (fd >= eventLoop->setsize) return;

    int old_mask = state->fd_to_mask[fd];
    int new_mask = old_mask & (~delmask);

    if (old_mask == AE_NONE) return;

    /* Remove the current poll operation */
    submit_poll_remove(&state->ring, (void*)(long)fd);

    if (new_mask != AE_NONE) {
        /* Re-add with new mask */
        submit_poll(&state->ring, fd, new_mask, (void*)(long)fd);
    }

    state->fd_to_mask[fd] = new_mask;

    /* Submit the operations */
    io_uring_submit(&state->ring);
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;
    struct __kernel_timespec ts, *tsp = NULL;

    /* Convert timeout to timespec */
    if (tvp) {
        ts.tv_sec = tvp->tv_sec;
        ts.tv_nsec = tvp->tv_usec * 1000;
        tsp = &ts;
    }

    /* Submit any pending operations first */
    io_uring_submit(&state->ring);

    /* Wait for completion events */
    struct io_uring_cqe *cqe;
    int ret;

    /* First, check for any immediately available completions */
    while (io_uring_peek_cqe(&state->ring, &cqe) == 0 && numevents < eventLoop->setsize) {
        void *user_data = io_uring_cqe_get_data(cqe);

        /* Handle completion */
        if ((unsigned long)user_data < (unsigned long)eventLoop->setsize) {
            /* This is a poll completion */
            int fd = (int)(long)user_data;
            int mask = 0;

            /* Check if this fd is still being monitored */
            if (fd >= 0 && fd < eventLoop->setsize && state->fd_to_mask[fd] != AE_NONE) {
                if (cqe->res & POLLIN) mask |= AE_READABLE;
                if (cqe->res & POLLOUT) mask |= AE_WRITABLE;
                if (cqe->res & POLLERR) mask |= AE_WRITABLE|AE_READABLE;
                if (cqe->res & POLLHUP) mask |= AE_WRITABLE|AE_READABLE;

                if (mask & state->fd_to_mask[fd]) {
                    eventLoop->fired[numevents].fd = fd;
                    eventLoop->fired[numevents].mask = mask & state->fd_to_mask[fd];
                    numevents++;
                }

                /* Re-arm the poll for this fd */
                submit_poll(&state->ring, fd, state->fd_to_mask[fd], (void*)(long)fd);
            }
        } else {
            /* This is an async I/O completion */
            uring_async_req *req = (uring_async_req*)user_data;
            if (req) {
                /* Call the appropriate callback */
                if (req->type == AE_ASYNC_READ && req->callback.read_cb) {
                    req->callback.read_cb(req->fd, cqe->res, req->buf, req->user_data);
                } else if (req->type == AE_ASYNC_WRITE && req->callback.write_cb) {
                    req->callback.write_cb(req->fd, cqe->res, req->user_data);
                } else if (req->type == AE_ASYNC_ACCEPT && req->callback.accept_cb) {
                    req->callback.accept_cb(req->fd, cqe->res, req->user_data);
                }

                /* Remove from pending requests list */
                uring_async_req **current = &state->async_requests;
                while (*current) {
                    if (*current == req) {
                        *current = req->next;
                        zfree(req);
                        break;
                    }
                    current = &(*current)->next;
                }
            }
        }

        io_uring_cqe_seen(&state->ring, cqe);
    }

    /* If we have events, return them */
    if (numevents > 0) {
        return numevents;
    }

    /* If no events and we don't want to wait, return 0 */
    if (tvp && tvp->tv_sec == 0 && tvp->tv_usec == 0) {
        return 0;
    }

    /* Wait for events using io_uring_wait_cqe_timeout */
    ret = io_uring_wait_cqe_timeout(&state->ring, &cqe, tsp);
    if (ret < 0) {
        if (ret != -ETIME && ret != -EINTR) {
            panic("aeApiPoll: io_uring_wait_cqe_timeout, %s", strerror(-ret));
        }
        return 0;
    }

    /* Process the completion we just waited for */
    if (cqe && numevents < eventLoop->setsize) {
        void *user_data = io_uring_cqe_get_data(cqe);

        /* Handle completion */
        if ((unsigned long)user_data < (unsigned long)eventLoop->setsize) {
            /* This is a poll completion */
            int fd = (int)(long)user_data;
            int mask = 0;

            /* Check if this fd is still being monitored */
            if (fd >= 0 && fd < eventLoop->setsize && state->fd_to_mask[fd] != AE_NONE) {
                if (cqe->res & POLLIN) mask |= AE_READABLE;
                if (cqe->res & POLLOUT) mask |= AE_WRITABLE;
                if (cqe->res & POLLERR) mask |= AE_WRITABLE|AE_READABLE;
                if (cqe->res & POLLHUP) mask |= AE_WRITABLE|AE_READABLE;

                if (mask & state->fd_to_mask[fd]) {
                    eventLoop->fired[numevents].fd = fd;
                    eventLoop->fired[numevents].mask = mask & state->fd_to_mask[fd];
                    numevents++;
                }

                /* Re-arm the poll for this fd */
                submit_poll(&state->ring, fd, state->fd_to_mask[fd], (void*)(long)fd);
            }
        }

        io_uring_cqe_seen(&state->ring, cqe);
    }

    return numevents;
}

static char *aeApiName(void) {
    return "uring";
}

/* ======================== Async I/O API Implementation ===================== */

/* Submit an async read operation */
int aeAsyncRead(aeEventLoop *eventLoop, int fd, void *buf, size_t len,
               aeAsyncReadProc *callback, void *user_data) {
    aeApiState *state = eventLoop->apidata;

    /* Allocate async request structure */
    uring_async_req *req = zmalloc(sizeof(uring_async_req));
    if (!req) return AE_ERR;

    req->type = AE_ASYNC_READ;
    req->fd = fd;
    req->buf = buf;
    req->len = len;
    req->callback.read_cb = callback;
    req->user_data = user_data;
    req->next = state->async_requests;
    state->async_requests = req;

    /* Submit read operation to io_uring using liburing */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    io_uring_prep_read(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, req);

    /* Submit the operation */
    if (io_uring_submit(&state->ring) < 0) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    return AE_OK;
}

/* Submit an async write operation */
int aeAsyncWrite(aeEventLoop *eventLoop, int fd, void *buf, size_t len,
                aeAsyncWriteProc *callback, void *user_data) {
    aeApiState *state = eventLoop->apidata;

    /* Allocate async request structure */
    uring_async_req *req = zmalloc(sizeof(uring_async_req));
    if (!req) return AE_ERR;

    req->type = AE_ASYNC_WRITE;
    req->fd = fd;
    req->buf = buf;
    req->len = len;
    req->callback.write_cb = callback;
    req->user_data = user_data;
    req->next = state->async_requests;
    state->async_requests = req;

    /* Submit write operation to io_uring using liburing */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    io_uring_prep_write(sqe, fd, buf, len, 0);
    io_uring_sqe_set_data(sqe, req);

    /* Submit the operation */
    if (io_uring_submit(&state->ring) < 0) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    return AE_OK;
}

/* Submit an async accept operation */
int aeAsyncAccept(aeEventLoop *eventLoop, int fd,
                 aeAsyncAcceptProc *callback, void *user_data) {
    aeApiState *state = eventLoop->apidata;

    /* Allocate async request structure */
    uring_async_req *req = zmalloc(sizeof(uring_async_req));
    if (!req) return AE_ERR;

    req->type = AE_ASYNC_ACCEPT;
    req->fd = fd;
    req->buf = NULL;
    req->len = 0;
    req->callback.accept_cb = callback;
    req->user_data = user_data;
    req->next = state->async_requests;
    state->async_requests = req;

    /* Submit accept operation to io_uring using liburing */
    struct io_uring_sqe *sqe = io_uring_get_sqe(&state->ring);
    if (!sqe) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    io_uring_prep_accept(sqe, fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, req);

    /* Submit the operation */
    if (io_uring_submit(&state->ring) < 0) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    return AE_OK;
}
