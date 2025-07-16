/* Linux io_uring based ae.c module
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <sys/syscall.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>

/* io_uring system call wrappers */
static int io_uring_setup(unsigned entries, struct io_uring_params *params) {
    return syscall(__NR_io_uring_setup, entries, params);
}

static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                         unsigned flags, sigset_t *sig) {
    return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, _NSIG/8);
}

/* io_uring ring structure */
typedef struct {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    unsigned *flags;
    unsigned *array;
    struct io_uring_sqe *sqes;
    size_t ring_sz;
    void *ring_ptr;
} io_uring_sq;

typedef struct {
    unsigned *head;
    unsigned *tail;
    unsigned *ring_mask;
    unsigned *ring_entries;
    struct io_uring_cqe *cqes;
    size_t ring_sz;
    void *ring_ptr;
} io_uring_cq;

typedef struct {
    int ring_fd;
    io_uring_sq sq;
    io_uring_cq cq;
} io_uring;

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
    io_uring ring;
    struct io_uring_cqe *events;       /* Completion events array */
    int *fd_to_mask;                    /* Map fd to event mask for polling */
    uring_async_req *async_requests;    /* Pending async requests */
} aeApiState;

/* Initialize io_uring */
static int setup_io_uring(io_uring *ring, unsigned entries) {
    struct io_uring_params params;
    int ret;
    
    memset(&params, 0, sizeof(params));
    ret = io_uring_setup(entries, &params);
    if (ret < 0) {
        return -1;
    }
    
    ring->ring_fd = ret;
    
    /* Map submission queue */
    ring->sq.ring_sz = params.sq_off.array + params.sq_entries * sizeof(unsigned);
    ring->sq.ring_ptr = mmap(0, ring->sq.ring_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring->ring_fd, IORING_OFF_SQ_RING);
    if (ring->sq.ring_ptr == MAP_FAILED) {
        close(ring->ring_fd);
        return -1;
    }
    
    ring->sq.head = (unsigned*)((char*)ring->sq.ring_ptr + params.sq_off.head);
    ring->sq.tail = (unsigned*)((char*)ring->sq.ring_ptr + params.sq_off.tail);
    ring->sq.ring_mask = (unsigned*)((char*)ring->sq.ring_ptr + params.sq_off.ring_mask);
    ring->sq.ring_entries = (unsigned*)((char*)ring->sq.ring_ptr + params.sq_off.ring_entries);
    ring->sq.flags = (unsigned*)((char*)ring->sq.ring_ptr + params.sq_off.flags);
    ring->sq.array = (unsigned*)((char*)ring->sq.ring_ptr + params.sq_off.array);
    
    /* Map submission queue entries */
    ring->sq.sqes = mmap(0, params.sq_entries * sizeof(struct io_uring_sqe),
                        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                        ring->ring_fd, IORING_OFF_SQES);
    if (ring->sq.sqes == MAP_FAILED) {
        munmap(ring->sq.ring_ptr, ring->sq.ring_sz);
        close(ring->ring_fd);
        return -1;
    }
    
    /* Map completion queue */
    ring->cq.ring_sz = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    ring->cq.ring_ptr = mmap(0, ring->cq.ring_sz, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE, ring->ring_fd, IORING_OFF_CQ_RING);
    if (ring->cq.ring_ptr == MAP_FAILED) {
        munmap(ring->sq.sqes, params.sq_entries * sizeof(struct io_uring_sqe));
        munmap(ring->sq.ring_ptr, ring->sq.ring_sz);
        close(ring->ring_fd);
        return -1;
    }
    
    ring->cq.head = (unsigned*)((char*)ring->cq.ring_ptr + params.cq_off.head);
    ring->cq.tail = (unsigned*)((char*)ring->cq.ring_ptr + params.cq_off.tail);
    ring->cq.ring_mask = (unsigned*)((char*)ring->cq.ring_ptr + params.cq_off.ring_mask);
    ring->cq.ring_entries = (unsigned*)((char*)ring->cq.ring_ptr + params.cq_off.ring_entries);
    ring->cq.cqes = (struct io_uring_cqe*)((char*)ring->cq.ring_ptr + params.cq_off.cqes);
    
    return 0;
}

/* Cleanup io_uring */
static void cleanup_io_uring(io_uring *ring) {
    if (ring->sq.ring_ptr != MAP_FAILED) {
        munmap(ring->sq.ring_ptr, ring->sq.ring_sz);
    }
    if (ring->sq.sqes != MAP_FAILED) {
        munmap(ring->sq.sqes, *ring->sq.ring_entries * sizeof(struct io_uring_sqe));
    }
    if (ring->cq.ring_ptr != MAP_FAILED) {
        munmap(ring->cq.ring_ptr, ring->cq.ring_sz);
    }
    if (ring->ring_fd >= 0) {
        close(ring->ring_fd);
    }
}

/* Submit a poll operation to io_uring */
static int submit_poll(io_uring *ring, int fd, int mask, void *user_data) {
    unsigned tail = *ring->sq.tail;
    unsigned index = tail & *ring->sq.ring_mask;
    struct io_uring_sqe *sqe = &ring->sq.sqes[index];
    
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->user_data = (unsigned long)user_data;
    
    /* Convert AE mask to poll events */
    __u32 poll_events = 0;
    if (mask & AE_READABLE) poll_events |= POLLIN;
    if (mask & AE_WRITABLE) poll_events |= POLLOUT;
    sqe->poll_events = poll_events;
    
    ring->sq.array[index] = index;
    *ring->sq.tail = tail + 1;
    
    return 0;
}

/* Remove a poll operation from io_uring */
static int submit_poll_remove(io_uring *ring, void *user_data) {
    unsigned tail = *ring->sq.tail;
    unsigned index = tail & *ring->sq.ring_mask;
    struct io_uring_sqe *sqe = &ring->sq.sqes[index];
    
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_POLL_REMOVE;
    sqe->addr = (unsigned long)user_data;
    
    ring->sq.array[index] = index;
    *ring->sq.tail = tail + 1;
    
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
        unsigned pending = *state->ring.sq.tail - *state->ring.sq.head;
        if (pending > 0) {
            io_uring_enter(state->ring.ring_fd, pending, 0, 0, NULL);
        }
    }

    /* Add new poll operation */
    if (submit_poll(&state->ring, fd, new_mask, (void*)(long)fd) < 0) {
        return -1;
    }

    state->fd_to_mask[fd] = new_mask;

    /* Submit the add operation */
    unsigned pending = *state->ring.sq.tail - *state->ring.sq.head;
    if (pending > 0) {
        if (io_uring_enter(state->ring.ring_fd, pending, 0, 0, NULL) < 0) {
            return -1;
        }
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
    unsigned pending = *state->ring.sq.tail - *state->ring.sq.head;
    if (pending > 0) {
        io_uring_enter(state->ring.ring_fd, pending, 0, 0, NULL);
    }
}

static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int numevents = 0;
    int timeout_ms = -1;

    /* Convert timeout to milliseconds */
    if (tvp) {
        timeout_ms = tvp->tv_sec * 1000 + (tvp->tv_usec + 999) / 1000;
    }

    /* Submit any pending operations first */
    unsigned pending = *state->ring.sq.tail - *state->ring.sq.head;
    if (pending > 0) {
        int ret = io_uring_enter(state->ring.ring_fd, pending, 0, 0, NULL);
        if (ret < 0 && errno != EINTR) {
            panic("aeApiPoll: io_uring_enter submit, %s", strerror(errno));
        }
    }

    /* Check for completion events first (non-blocking) */
    unsigned head = *state->ring.cq.head;
    unsigned tail = *state->ring.cq.tail;

    /* Process any available completions */
    while (head != tail && numevents < eventLoop->setsize) {
        unsigned index = head & *state->ring.cq.ring_mask;
        struct io_uring_cqe *cqe = &state->ring.cq.cqes[index];

        /* Handle completion */
        if (cqe->user_data < (unsigned long)eventLoop->setsize) {
            /* This is a poll completion */
            int fd = (int)cqe->user_data;
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
            uring_async_req *req = (uring_async_req*)cqe->user_data;
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

        head++;
    }

    /* Update completion queue head */
    *state->ring.cq.head = head;

    /* If we have events, return them */
    if (numevents > 0) {
        return numevents;
    }

    /* If no events and we don't want to wait, return 0 */
    if (timeout_ms == 0) {
        return 0;
    }

    /* Wait for events using io_uring_enter with timeout */
    int ret = io_uring_enter(state->ring.ring_fd, 0, 1, IORING_ENTER_GETEVENTS, NULL);
    if (ret < 0) {
        if (errno != EINTR) {
            panic("aeApiPoll: io_uring_enter wait, %s", strerror(errno));
        }
        return 0;
    }

    /* Process any new completions after waiting */
    head = *state->ring.cq.head;
    tail = *state->ring.cq.tail;

    while (head != tail && numevents < eventLoop->setsize) {
        unsigned index = head & *state->ring.cq.ring_mask;
        struct io_uring_cqe *cqe = &state->ring.cq.cqes[index];

        /* Handle completion */
        if (cqe->user_data < (unsigned long)eventLoop->setsize) {
            /* This is a poll completion */
            int fd = (int)cqe->user_data;
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

        head++;
    }

    /* Update completion queue head */
    *state->ring.cq.head = head;

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

    /* Submit read operation to io_uring */
    unsigned tail = *state->ring.sq.tail;
    unsigned index = tail & *state->ring.sq.ring_mask;
    struct io_uring_sqe *sqe = &state->ring.sq.sqes[index];

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_READ;
    sqe->fd = fd;
    sqe->addr = (unsigned long)buf;
    sqe->len = len;
    sqe->user_data = (unsigned long)req;

    state->ring.sq.array[index] = index;
    *state->ring.sq.tail = tail + 1;

    /* Submit the operation */
    if (io_uring_enter(state->ring.ring_fd, 1, 0, 0, NULL) < 0) {
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

    /* Submit write operation to io_uring */
    unsigned tail = *state->ring.sq.tail;
    unsigned index = tail & *state->ring.sq.ring_mask;
    struct io_uring_sqe *sqe = &state->ring.sq.sqes[index];

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_WRITE;
    sqe->fd = fd;
    sqe->addr = (unsigned long)buf;
    sqe->len = len;
    sqe->user_data = (unsigned long)req;

    state->ring.sq.array[index] = index;
    *state->ring.sq.tail = tail + 1;

    /* Submit the operation */
    if (io_uring_enter(state->ring.ring_fd, 1, 0, 0, NULL) < 0) {
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

    /* Submit accept operation to io_uring */
    unsigned tail = *state->ring.sq.tail;
    unsigned index = tail & *state->ring.sq.ring_mask;
    struct io_uring_sqe *sqe = &state->ring.sq.sqes[index];

    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = fd;
    sqe->addr = 0;  /* No sockaddr for now */
    sqe->addr2 = 0; /* No addrlen for now */
    sqe->user_data = (unsigned long)req;

    state->ring.sq.array[index] = index;
    *state->ring.sq.tail = tail + 1;

    /* Submit the operation */
    if (io_uring_enter(state->ring.ring_fd, 1, 0, 0, NULL) < 0) {
        /* Remove from pending requests on error */
        state->async_requests = req->next;
        zfree(req);
        return AE_ERR;
    }

    return AE_OK;
}
