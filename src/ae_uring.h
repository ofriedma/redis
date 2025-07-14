/* Linux io_uring based ae.c module header
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef __AE_URING_H__
#define __AE_URING_H__

#ifdef HAVE_LIBURING

/* Avoid UNUSED macro conflict with liburing */
#ifdef UNUSED
#undef UNUSED
#endif

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

/* Restore Redis UNUSED macro after liburing include */
#ifndef UNUSED
#define UNUSED(V) ((void) V)
#endif

/* Forward declarations */
struct connection;
typedef struct connection connection;
typedef struct list list;

/* io_uring capability flags */
#define URING_CAP_NONE          0
#define URING_CAP_BASIC         1
#define URING_CAP_SQPOLL        2
#define URING_CAP_BUFFER_RING   4
#define URING_CAP_MULTISHOT     8

/* Operation types */
#define URING_OP_READ           1
#define URING_OP_WRITE          2
#define URING_OP_ACCEPT         3
#define URING_OP_RECV           4
#define URING_OP_SEND           5
#define URING_OP_WAKEUP         6

/* Configuration defaults */
#define URING_DEFAULT_SQ_ENTRIES        512
#define URING_DEFAULT_CQ_ENTRIES        1024
#define URING_DEFAULT_BUFFER_COUNT      1024
#define URING_DEFAULT_BUFFER_SIZE       4096
#define URING_DEFAULT_BATCH_SIZE        32
#define URING_DEFAULT_SQPOLL_IDLE       1000
#define URING_MAX_BATCH_SIZE            128

/* Special markers */
#define WAKEUP_MARKER                   ((void*)0xDEADBEEF)
#define BUFFER_RING_ID                  0

/* Buffer ring for zero-copy operations */
typedef struct uring_buffer_ring {
    struct io_uring_buf_ring *br;
    void *buffer_base;
    int buffer_count;
    int buffer_size;
    int buffer_mask;
    int next_buffer_id;
} uring_buffer_ring;

/* Per-operation context */
typedef struct uring_op_context {
    int fd;
    int op_type;
    int mask;                           /* AE_READABLE | AE_WRITABLE */
    void *user_data;
    void *callback;                     /* Generic callback pointer */

    /* Buffer management */
    void *buffer;
    size_t buffer_size;
    int buffer_id;                      /* For buffer ring */

    /* Timing */
    monotime submit_time;

    /* Operation state tracking */
    struct {
        int active;
    } read_op, write_op, accept_op;

    /* Linked operations */
    struct uring_op_context *next;
} uring_op_context;

/* Per-connection context for io_uring */
typedef struct uring_conn_context {
    connection *conn;
    int pending_ops;                    /* Number of pending operations */
    
    struct {
        int active;
        uring_op_context *ctx;
    } read_op, write_op;
    
    /* Operation queues for batching */
    list *pending_reads;
    list *pending_writes;
    
    /* Statistics */
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t ops_completed;
    uint64_t ops_failed;
} uring_conn_context;

/* Buffer pool entry for efficient memory management */
typedef struct uring_buffer_entry {
    void *buffer;                       /* Pointer to the buffer memory */
    size_t size;                        /* Size of the buffer */
    int in_use;                         /* 1 if buffer is currently allocated, 0 if free */
    uint64_t last_used;                 /* Timestamp of last use (for LRU) */
    struct uring_buffer_entry *next;    /* Next entry in free list */
} uring_buffer_entry;

/* Buffer pool for managing pre-allocated buffers */
typedef struct uring_buffer_pool {
    uring_buffer_entry *entries;        /* Array of buffer entries */
    uring_buffer_entry *free_list;      /* Head of free buffer list */
    int pool_size;                      /* Total number of buffers in pool */
    int buffer_size;                    /* Size of each individual buffer */
    int free_count;                     /* Number of free buffers */
    int allocated_count;                /* Number of allocated buffers */

    /* Statistics */
    uint64_t total_allocations;         /* Total allocation requests */
    uint64_t pool_hits;                 /* Successful allocations from pool */
    uint64_t pool_misses;               /* Failed allocations (pool exhausted) */
    uint64_t total_deallocations;       /* Total deallocation requests */
    uint64_t peak_usage;                /* Peak number of buffers in use */

    /* Thread safety */
    pthread_mutex_t mutex;              /* Mutex for thread-safe operations */
} uring_buffer_pool;

/* Main io_uring state */
typedef struct aeApiState {
    struct io_uring ring;               /* io_uring instance */
    int ring_fd;                        /* io_uring file descriptor */
    int capabilities;                   /* Detected capabilities */
    
    /* Queue configuration */
    int sq_entries;                     /* Submission queue size */
    int cq_entries;                     /* Completion queue size */
    
    /* SQPOLL support */
    int sqpoll_enabled;                 /* SQPOLL active */
    int sqpoll_cpu;                     /* CPU for SQPOLL kernel thread */
    int sqpoll_idle_ms;                 /* SQPOLL idle timeout */
    
    /* Wake-up mechanism */
    int wakeup_eventfd;                 /* eventfd for waking SQPOLL */
    uring_op_context *wakeup_ctx;       /* Wake-up operation context */
    uint64_t wakeup_data;               /* Wake-up data buffer */
    
    /* Buffer management */
    uring_buffer_ring buf_ring;         /* Buffer ring for zero-copy */
    int buffer_ring_enabled;
    uring_buffer_pool *buffer_pool;     /* Pre-allocated buffer pool */
    
    /* Event tracking */
    uring_op_context **contexts;       /* Operation contexts by FD */
    int max_contexts;                   /* Maximum tracked contexts */
    
    /* Batch submission */
    struct io_uring_sqe *batch_sqes[URING_MAX_BATCH_SIZE];
    int batch_count;
    pthread_mutex_t batch_lock;
    
    /* Statistics */
    struct {
        uint64_t ops_submitted;
        uint64_t ops_completed;
        uint64_t ops_failed;
        uint64_t total_submit_time_us;
        uint64_t total_completion_time_us;
        uint64_t max_completion_time_us;
        uint64_t sq_full_count;
        uint64_t cq_overflow_count;
        uint64_t sqpoll_wakeups;
        uint64_t sqpoll_idle_time_us;
        uint64_t buffer_ring_hits;
        uint64_t buffer_ring_misses;
    } stats;
} aeApiState;

/* Configuration structure */
typedef struct {
    int enabled;                        /* Enable io_uring */
    int sqpoll_enabled;                 /* Enable SQPOLL */
    int sqpoll_cpu;                     /* SQPOLL CPU affinity */
    int sqpoll_idle_ms;                 /* SQPOLL idle timeout */
    int sq_entries;                     /* Submission queue size */
    int cq_entries;                     /* Completion queue size */
    int buffer_ring_size;               /* Buffer ring size */
    int buffer_size;                    /* Individual buffer size */
    int batch_submit_size;              /* Batch submission size */
    int multishot_accept;               /* Use multishot accept */
    int multishot_recv;                 /* Use multishot recv */
    int linked_ops;                     /* Use linked operations */
} uring_config;

/* Function prototypes */

/* Public interface functions */
void aeGetUringStats(aeEventLoop *eventLoop, char **info);

/* Buffer pool management functions */
uring_buffer_pool *create_buffer_pool(int pool_size, int buffer_size);
void *get_buffer_from_pool(uring_buffer_pool *pool);
void return_buffer_to_pool(uring_buffer_pool *pool, void *buffer);
void free_buffer_pool(uring_buffer_pool *pool);
void get_buffer_pool_stats(uring_buffer_pool *pool, char **info);

/* Functions needed by socket.c */
uring_op_context *create_op_context(int fd, int op_type, int mask);
int submit_read_operation(aeApiState *state, int fd, uring_op_context *ctx);
int submit_accept_operation(aeApiState *state, int fd, uring_op_context *ctx);

#endif /* HAVE_LIBURING */
#endif /* __AE_URING_H__ */
