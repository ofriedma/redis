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

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

/* Forward declarations */
struct connection;
typedef struct connection connection;
typedef struct list list;

/* io_uring capability flags */
#define URING_CAP_NONE              0x0000
#define URING_CAP_BASIC             0x0001
#define URING_CAP_SQPOLL            0x0002
#define URING_CAP_BUFFER_RING       0x0004
#define URING_CAP_MULTISHOT         0x0008
#define URING_CAP_SINGLE_MMAP       0x0010
#define URING_CAP_NODROP            0x0020
#define URING_CAP_SUBMIT_STABLE     0x0040
#define URING_CAP_RW_CUR_POS        0x0080
#define URING_CAP_CUR_PERSONALITY   0x0100
#define URING_CAP_FAST_POLL         0x0200

/* Operation types */
#define URING_OP_READ           1
#define URING_OP_WRITE          2
#define URING_OP_ACCEPT         3
#define URING_OP_RECV           4
#define URING_OP_SEND           5


/* Configuration defaults */
#define URING_DEFAULT_SQ_ENTRIES        512
#define URING_DEFAULT_CQ_ENTRIES        1024
#define URING_DEFAULT_BUFFER_COUNT      1024
#define URING_DEFAULT_BUFFER_SIZE       4096
#define URING_DEFAULT_BATCH_SIZE        32
#define URING_DEFAULT_SQPOLL_IDLE       1000
#define URING_MAX_BATCH_SIZE            128


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

/* Operation lifecycle states */
typedef enum {
    URING_OP_STATE_INIT = 0,
    URING_OP_STATE_QUEUED,
    URING_OP_STATE_SUBMITTED,
    URING_OP_STATE_PROCESSING,
    URING_OP_STATE_COMPLETED,
    URING_OP_STATE_FAILED,
    URING_OP_STATE_CANCELLED,
    URING_OP_STATE_TIMEOUT
} uring_op_state;

/* Operation priority levels */
typedef enum {
    URING_OP_PRIORITY_LOW = 0,
    URING_OP_PRIORITY_NORMAL,
    URING_OP_PRIORITY_HIGH,
    URING_OP_PRIORITY_CRITICAL
} uring_op_priority;

/* Enhanced per-operation context */
typedef struct uring_op_context {
    /* Basic operation info */
    int fd;
    int op_type;
    int mask;                           /* AE_READABLE | AE_WRITABLE */
    uring_op_state state;
    uring_op_priority priority;

    /* Operation identification */
    uint64_t op_id;                     /* Unique operation ID */
    uint32_t sequence_num;              /* Sequence number for ordering */

    /* User data and callbacks */
    void *user_data;
    void *callback;                     /* Generic callback pointer */
    void (*completion_callback)(struct uring_op_context *ctx, int result);
    void (*error_callback)(struct uring_op_context *ctx, int error);

    /* Buffer management */
    void *buffer;
    size_t buffer_size;
    int buffer_id;                      /* For buffer ring */
    size_t bytes_transferred;           /* Actual bytes read/written */

    /* Timing and performance */
    monotime submit_time;
    monotime completion_time;
    uint64_t timeout_us;                /* Operation timeout */
    uint32_t retry_count;
    uint32_t max_retries;

    /* Operation state tracking */
    struct {
        int active;
        int retry_count;
        monotime last_attempt;
    } read_op, write_op, accept_op;

    /* Error handling */
    int last_error;
    char error_msg[256];

    /* Resource tracking */
    struct uring_conn_context *conn_ctx; /* Associated connection */
    struct aeEventLoop *event_loop;      /* Associated event loop */

    /* Linked operations for batching and cleanup */
    struct uring_op_context *next;
    struct uring_op_context *prev;

    /* Reference counting for safe cleanup */
    int ref_count;
    pthread_mutex_t ref_lock;
} uring_op_context;

/* Connection state for lifecycle management */
typedef enum {
    URING_CONN_INIT = 0,
    URING_CONN_CONNECTING,
    URING_CONN_CONNECTED,
    URING_CONN_READING,
    URING_CONN_WRITING,
    URING_CONN_CLOSING,
    URING_CONN_CLOSED,
    URING_CONN_ERROR
} uring_connection_state;

/* Enhanced per-connection context for io_uring */
typedef struct uring_conn_context {
    connection *conn;
    int fd;
    uring_connection_state state;
    int pending_ops;                    /* Number of pending operations */

    /* Lifecycle tracking */
    uint64_t created_time;
    uint64_t last_activity;
    uint64_t state_change_time;
    uint32_t error_count;
    uint32_t timeout_count;

    /* Address information */
    struct sockaddr_storage remote_addr;
    socklen_t remote_addr_len;
    struct sockaddr_storage local_addr;
    socklen_t local_addr_len;

    struct {
        int active;
        uring_op_context *ctx;
        uint64_t last_submit_time;
        uint32_t retry_count;
    } read_op, write_op, accept_op;

    /* Operation queues for batching */
    list *pending_reads;
    list *pending_writes;

    /* Connection-specific buffers */
    void *read_buffer;
    void *write_buffer;
    int read_buffer_size;
    int write_buffer_size;

    /* Statistics */
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t ops_completed;
    uint64_t ops_failed;
    uint64_t ops_submitted;

    /* Linked list for connection tracking */
    struct uring_conn_context *next;
    struct uring_conn_context *prev;
} uring_conn_context;

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
    

    
    /* Buffer management */
    uring_buffer_ring buf_ring;         /* Buffer ring for zero-copy */
    int buffer_ring_enabled;
    
    /* Event tracking */
    uring_op_context **contexts;       /* Operation contexts by FD */
    int max_contexts;                   /* Maximum tracked contexts */

    /* Operation tracking and management */
    uring_op_context *op_list_head;     /* Head of active operations list */
    uring_op_context *op_list_tail;     /* Tail of active operations list */
    uint64_t next_op_id;                /* Next operation ID to assign */
    uint32_t next_sequence_num;         /* Next sequence number */
    int active_operations;              /* Count of active operations */
    int max_operations;                 /* Maximum concurrent operations */
    pthread_mutex_t op_lock;            /* Lock for operation list */

    /* Operation queues by priority */
    struct {
        uring_op_context *head;
        uring_op_context *tail;
        int count;
    } priority_queues[4];               /* One for each priority level */

    /* Connection lifecycle management */
    uring_conn_context **connections;  /* Connection contexts by FD */
    uring_conn_context *conn_list_head; /* Head of connection list */
    uring_conn_context *conn_list_tail; /* Tail of connection list */
    int max_connections;                /* Maximum tracked connections */
    int active_connections;             /* Current active connections */
    pthread_mutex_t conn_lock;          /* Lock for connection list */
    
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

        uint64_t sqpoll_idle_time_us;
        uint64_t buffer_ring_hits;
        uint64_t buffer_ring_misses;

        /* Operation type specific stats */
        uint64_t read_ops;
        uint64_t write_ops;
        uint64_t accept_ops;
        uint64_t recv_ops;
        uint64_t send_ops;

        /* Error type specific stats */
        uint64_t eagain_errors;
        uint64_t econnreset_errors;
        uint64_t epipe_errors;
        uint64_t other_errors;

        /* Performance metrics */
        uint64_t batch_submissions;
        uint64_t single_submissions;
        uint64_t poll_calls;
        uint64_t poll_timeouts;

        /* Memory usage */
        uint64_t buffer_allocations;
        uint64_t buffer_deallocations;
        uint64_t context_allocations;
        uint64_t context_deallocations;
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

/* Functions needed by socket.c */
uring_op_context *create_op_context(int fd, int op_type, int mask);
int submit_read_operation(aeApiState *state, int fd, uring_op_context *ctx);
int submit_accept_operation(aeApiState *state, int fd, uring_op_context *ctx);

/* Connection lifecycle management functions */
uring_conn_context *create_connection_context(aeApiState *state, int fd, connection *conn);
void destroy_connection_context(aeApiState *state, uring_conn_context *conn_ctx);
uring_conn_context *get_connection_context(aeApiState *state, int fd);
void update_connection_state(uring_conn_context *conn_ctx, uring_connection_state new_state);
void update_connection_activity(uring_conn_context *conn_ctx);
void cleanup_connection_operations(aeApiState *state, uring_conn_context *conn_ctx);
void get_connection_stats(aeApiState *state, char **info);

/* Operation lifecycle management functions */
uring_op_context *create_operation_context(aeApiState *state, int fd, int op_type, uring_op_priority priority);
void destroy_operation_context(aeApiState *state, uring_op_context *op_ctx);
void add_operation_to_queue(aeApiState *state, uring_op_context *op_ctx);
uring_op_context *get_next_operation(aeApiState *state, uring_op_priority min_priority);
void update_operation_state(uring_op_context *op_ctx, uring_op_state new_state);
void cancel_operation(aeApiState *state, uring_op_context *op_ctx);
void timeout_operations(aeApiState *state);
int retry_operation(aeApiState *state, uring_op_context *op_ctx);
void cleanup_completed_operations(aeApiState *state);
void get_operation_stats(aeApiState *state, char **info);

/* Operation reference counting */
void op_context_ref(uring_op_context *op_ctx);
void op_context_unref(uring_op_context *op_ctx);

/* Operation resource management */
void set_operation_timeout(uring_op_context *op_ctx, uint64_t timeout_us);
void set_operation_callback(uring_op_context *op_ctx,
                           void (*completion_cb)(uring_op_context *, int),
                           void (*error_cb)(uring_op_context *, int));

#endif /* HAVE_LIBURING */
#endif /* __AE_URING_H__ */
