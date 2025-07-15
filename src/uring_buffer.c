/* io_uring buffer management for Redis
 *
 * Copyright (c) 2024-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "ae.h"

#ifdef HAVE_LIBURING

#include "ae_uring.h"
#include "zmalloc.h"

#ifndef REDIS_CLI_BUILD
#include "server.h"
#include "sds.h"
#endif

#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations for buffer pool functions */
int init_buffer_pool(int count, int size);
void *get_buffer_from_pool(void);
void *get_buffer_from_pool_sized(size_t size);
void return_buffer_to_pool(void *buffer);
void cleanup_buffer_pool(void);
void get_buffer_pool_stats(char **info);

/* Forward declarations for zero-copy buffer functions */
void *get_buffer_from_completion(aeApiState *state, struct io_uring_cqe *cqe, int *buffer_id, int *buffer_size);
int register_uring_buffers(aeApiState *state, struct iovec *iovecs, int count);
int unregister_uring_buffers(aeApiState *state);

/* Forward declarations for memory optimization functions */
void optimize_buffer_pool(void);
void *allocate_optimized_buffer(size_t size);
void monitor_memory_usage(void);

/* Setup buffer ring for zero-copy operations */
int setup_buffer_ring(aeApiState *state) {
    #ifndef REDIS_CLI_BUILD
    int buffer_count = server.uring_config.buffer_ring_size;
    int buffer_size = server.uring_config.buffer_size;
    #else
    int buffer_count = 1024;  /* Default for CLI builds */
    int buffer_size = 4096;   /* Default for CLI builds */
    #endif
    
    /* Ensure buffer count is power of 2 */
    if (buffer_count & (buffer_count - 1)) {
        /* Round up to next power of 2 */
        buffer_count--;
        buffer_count |= buffer_count >> 1;
        buffer_count |= buffer_count >> 2;
        buffer_count |= buffer_count >> 4;
        buffer_count |= buffer_count >> 8;
        buffer_count |= buffer_count >> 16;
        buffer_count++;
    }
    
    state->buf_ring.buffer_count = buffer_count;
    state->buf_ring.buffer_size = buffer_size;
    state->buf_ring.buffer_mask = buffer_count - 1;
    state->buf_ring.next_buffer_id = 0;
    
    /* Allocate buffer ring structure - use a reasonable size estimate */
    size_t ring_size = 64 * buffer_count; /* Estimate for buffer ring entry size */
    state->buf_ring.br = mmap(NULL, ring_size, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (state->buf_ring.br == MAP_FAILED) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to allocate io_uring buffer ring: %s", strerror(errno));
        #endif
        return -1;
    }
    
    /* Allocate actual data buffers */
    size_t total_buffer_size = (size_t)buffer_count * buffer_size;
    state->buf_ring.buffer_base = mmap(NULL, total_buffer_size, PROT_READ | PROT_WRITE,
                                       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (state->buf_ring.buffer_base == MAP_FAILED) {
        munmap(state->buf_ring.br, ring_size);
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to allocate io_uring data buffers: %s", strerror(errno));
        #endif
        return -1;
    }
    
    /* Try to register buffer ring with io_uring for zero-copy operations */
    #ifdef IORING_REGISTER_PBUF_RING
    struct io_uring_buf_reg reg = {0};
    reg.ring_addr = (unsigned long)state->buf_ring.br;
    reg.ring_entries = buffer_count;
    reg.bgid = 0; /* Buffer group ID */

    int ret = io_uring_register_buf_ring(&state->ring, &reg, 0);
    if (ret == 0) {
        /* Buffer ring registration successful */
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_NOTICE, "io_uring buffer ring registered successfully: %d buffers of %d bytes each",
                  buffer_count, buffer_size);
        #endif

        /* Initialize buffer ring entries */
        for (int i = 0; i < buffer_count; i++) {
            void *buffer_addr = (char*)state->buf_ring.buffer_base + (i * buffer_size);
            io_uring_buf_ring_add(state->buf_ring.br, buffer_addr, buffer_size, i,
                                   io_uring_buf_ring_mask(buffer_count), i);
        }
        io_uring_buf_ring_advance(state->buf_ring.br, buffer_count);

        return 0;
    } else {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Buffer ring registration failed: %s - falling back to buffer pool",
                  strerror(-ret));
        #endif
    }
    #else
    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "Buffer ring not supported by liburing version - using buffer pool");
    #endif
    #endif

    /* Clean up allocated memory since buffer ring registration failed */
    munmap(state->buf_ring.buffer_base, total_buffer_size);
    munmap(state->buf_ring.br, ring_size);

    /* Initialize buffer pool instead */
    return init_buffer_pool(buffer_count, buffer_size);

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "io_uring buffer ring initialized: %d buffers of %d bytes each",
              buffer_count, buffer_size);
    #endif
    
    return 0;
}

/* Cleanup buffer ring */
void cleanup_buffer_ring(aeApiState *state) {
    if (!state->buffer_ring_enabled) {
        cleanup_buffer_pool();
        return;
    }

    /* Clean up buffer pool since we're using that instead of buffer rings */
    cleanup_buffer_pool();

    memset(&state->buf_ring, 0, sizeof(state->buf_ring));
    state->buffer_ring_enabled = 0;
}

/* Get buffer from ring for zero-copy operations */
void *get_buffer_from_ring(aeApiState *state, int *buffer_id) {
    if (!state->buffer_ring_enabled) {
        *buffer_id = -1;
        return get_buffer_from_pool();
    }

    /* For actual buffer rings, the kernel manages buffer allocation
     * We just need to provide the buffer group ID in the SQE */
    *buffer_id = state->buf_ring.next_buffer_id;
    state->buf_ring.next_buffer_id = (state->buf_ring.next_buffer_id + 1) & state->buf_ring.buffer_mask;

    /* Return a placeholder - actual buffer will be provided by kernel in CQE */
    return (void*)0x1; /* Non-NULL placeholder to indicate buffer ring usage */
}

/* Get buffer from completion for zero-copy operations */
void *get_buffer_from_completion(aeApiState *state, struct io_uring_cqe *cqe, int *buffer_id, int *buffer_size) {
    if (!state->buffer_ring_enabled) {
        *buffer_id = -1;
        *buffer_size = 0;
        return NULL;
    }

    /* Extract buffer information from CQE flags */
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        *buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        *buffer_size = state->buf_ring.buffer_size;

        /* Calculate buffer address */
        void *buffer = (char*)state->buf_ring.buffer_base + (*buffer_id * state->buf_ring.buffer_size);

        state->stats.buffer_ring_hits++;
        return buffer;
    }

    state->stats.buffer_ring_misses++;
    *buffer_id = -1;
    *buffer_size = 0;
    return NULL;
}

/* Return buffer to ring for zero-copy operations */
void return_buffer_to_ring(aeApiState *state, int buffer_id) {
    if (!state->buffer_ring_enabled || buffer_id < 0) {
        return; /* Not a buffer ring buffer */
    }

    /* For buffer rings, we need to add the buffer back to the ring */
    void *buffer_addr = (char*)state->buf_ring.buffer_base + (buffer_id * state->buf_ring.buffer_size);

    /* Add buffer back to the ring */
    io_uring_buf_ring_add(state->buf_ring.br, buffer_addr, state->buf_ring.buffer_size,
                           buffer_id, io_uring_buf_ring_mask(state->buf_ring.buffer_count), 0);
    io_uring_buf_ring_advance(state->buf_ring.br, 1);
}

/* Register additional buffers for zero-copy operations */
int register_uring_buffers(aeApiState *state, struct iovec *iovecs, int count) {
    if (!state || !iovecs || count <= 0) {
        return -1;
    }

    /* Register buffers with io_uring for zero-copy operations */
    int ret = io_uring_register_buffers(&state->ring, iovecs, count);
    if (ret < 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to register io_uring buffers: %s", strerror(-ret));
        #endif
        return -1;
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Registered %d buffers for zero-copy operations", count);
    #endif

    return 0;
}

/* Unregister buffers */
int unregister_uring_buffers(aeApiState *state) {
    if (!state) {
        return -1;
    }

    int ret = io_uring_unregister_buffers(&state->ring);
    if (ret < 0) {
        #ifndef REDIS_CLI_BUILD
        serverLog(LL_WARNING, "Failed to unregister io_uring buffers: %s", strerror(-ret));
        #endif
        return -1;
    }

    return 0;
}

/* Allocate regular buffer (fallback when buffer ring is not available) */
void *allocate_uring_buffer(size_t size) {
    return zmalloc(size);
}

/* Free regular buffer */
void free_uring_buffer(void *buffer) {
    if (buffer) {
        zfree(buffer);
    }
}

/* Get buffer statistics */
void get_buffer_ring_stats(aeApiState *state, char **info) {
    #ifndef REDIS_CLI_BUILD
    if (!state->buffer_ring_enabled) {
        *info = sdscatprintf(*info, "buffer_ring_enabled:no\r\n");
        return;
    }

    *info = sdscatprintf(*info,
        "buffer_ring_enabled:no\r\n"
        "buffer_pool_enabled:yes\r\n"
        "buffer_ring_hits:%lu\r\n"
        "buffer_ring_misses:%lu\r\n"
        "buffer_ring_hit_rate:%.2f\r\n",
        (unsigned long)state->stats.buffer_ring_hits,
        (unsigned long)state->stats.buffer_ring_misses,
        (state->stats.buffer_ring_hits + state->stats.buffer_ring_misses) > 0 ?
            (double)state->stats.buffer_ring_hits /
            (state->stats.buffer_ring_hits + state->stats.buffer_ring_misses) * 100.0 : 0.0);
    #endif
}

/* Enhanced buffer pool for non-ring operations */
typedef struct buffer_pool {
    void **buffers;
    int *available;
    int count;
    int size;
    int next_free;
    pthread_mutex_t lock;

    /* Enhanced features */
    int alignment;              /* Memory alignment requirement */
    int max_size;              /* Maximum pool size */
    int grow_threshold;        /* When to grow the pool */
    int shrink_threshold;      /* When to shrink the pool */

    /* Statistics */
    struct {
        uint64_t hits;
        uint64_t misses;
        uint64_t allocations;
        uint64_t deallocations;
        uint64_t grows;
        uint64_t shrinks;
        uint64_t peak_usage;
        uint64_t current_usage;
        uint64_t total_allocated_bytes;
        uint64_t total_freed_bytes;
    } stats;

    /* Multiple buffer sizes support */
    struct buffer_size_pool {
        int size;
        void **buffers;
        int *available;
        int count;
        int next_free;
        uint64_t hits;
        uint64_t misses;
    } *size_pools;
    int num_size_pools;
} buffer_pool;

static buffer_pool *global_buffer_pool = NULL;

/* Helper function to allocate aligned memory */
static void *allocate_aligned_buffer(size_t size, int alignment) {
    void *ptr = NULL;

    if (alignment <= 0 || (alignment & (alignment - 1)) != 0) {
        /* Invalid alignment, use regular allocation */
        return zmalloc(size);
    }

    /* Use posix_memalign for aligned allocation */
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return zmalloc(size); /* Fallback to regular allocation */
    }

    return ptr;
}

/* Initialize buffer pool with enhanced features */
int init_buffer_pool(int count, int size) {
    if (global_buffer_pool) {
        return 0; /* Already initialized */
    }

    global_buffer_pool = zmalloc(sizeof(buffer_pool));
    if (!global_buffer_pool) {
        return -1;
    }

    memset(global_buffer_pool, 0, sizeof(buffer_pool));

    /* Configure pool parameters */
    global_buffer_pool->count = count;
    global_buffer_pool->size = size;
    global_buffer_pool->alignment = 4096; /* Page alignment for better performance */
    global_buffer_pool->max_size = count * 4; /* Allow growth up to 4x initial size */
    global_buffer_pool->grow_threshold = count * 0.8; /* Grow when 80% full */
    global_buffer_pool->shrink_threshold = count * 0.2; /* Shrink when 20% used */

    global_buffer_pool->buffers = zmalloc(sizeof(void*) * count);
    global_buffer_pool->available = zmalloc(sizeof(int) * count);

    if (!global_buffer_pool->buffers || !global_buffer_pool->available) {
        cleanup_buffer_pool();
        return -1;
    }

    /* Allocate buffers with alignment */
    for (int i = 0; i < count; i++) {
        global_buffer_pool->buffers[i] = allocate_aligned_buffer(size, global_buffer_pool->alignment);
        if (!global_buffer_pool->buffers[i]) {
            /* Clean up allocated buffers */
            for (int j = 0; j < i; j++) {
                zfree(global_buffer_pool->buffers[j]); /* Use zfree for aligned memory */
            }
            cleanup_buffer_pool();
            return -1;
        }
        global_buffer_pool->available[i] = 1;
        global_buffer_pool->stats.allocations++;
        global_buffer_pool->stats.total_allocated_bytes += size;
    }

    global_buffer_pool->next_free = 0;
    pthread_mutex_init(&global_buffer_pool->lock, NULL);

    /* Initialize common buffer sizes (4KB, 8KB, 16KB, 64KB) */
    int common_sizes[] = {4096, 8192, 16384, 65536};
    global_buffer_pool->num_size_pools = sizeof(common_sizes) / sizeof(common_sizes[0]);
    global_buffer_pool->size_pools = zmalloc(sizeof(struct buffer_size_pool) * global_buffer_pool->num_size_pools);

    if (global_buffer_pool->size_pools) {
        for (int i = 0; i < global_buffer_pool->num_size_pools; i++) {
            struct buffer_size_pool *pool = &global_buffer_pool->size_pools[i];
            pool->size = common_sizes[i];
            pool->count = count / 4; /* Smaller pools for different sizes */
            pool->buffers = zmalloc(sizeof(void*) * pool->count);
            pool->available = zmalloc(sizeof(int) * pool->count);

            if (pool->buffers && pool->available) {
                for (int j = 0; j < pool->count; j++) {
                    pool->buffers[j] = allocate_aligned_buffer(pool->size, global_buffer_pool->alignment);
                    pool->available[j] = pool->buffers[j] ? 1 : 0;
                }
            }
        }
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "Enhanced buffer pool initialized: %d buffers of %d bytes, alignment=%d, max_size=%d",
              count, size, global_buffer_pool->alignment, global_buffer_pool->max_size);
    #endif

    return 0;
}

/* Get buffer from pool with size preference */
static void *get_buffer_from_size_pool(size_t requested_size) {
    if (!global_buffer_pool || !global_buffer_pool->size_pools) {
        return NULL;
    }

    /* Find best matching size pool */
    for (int i = 0; i < global_buffer_pool->num_size_pools; i++) {
        struct buffer_size_pool *pool = &global_buffer_pool->size_pools[i];
        if (pool->size >= requested_size) {
            /* Try to get buffer from this size pool */
            for (int j = 0; j < pool->count; j++) {
                int idx = (pool->next_free + j) % pool->count;
                if (pool->available[idx]) {
                    pool->available[idx] = 0;
                    pool->next_free = (idx + 1) % pool->count;
                    pool->hits++;
                    return pool->buffers[idx];
                }
            }
            pool->misses++;
            break; /* Found right size pool but no available buffers */
        }
    }

    return NULL;
}

/* Enhanced get buffer from pool */
void *get_buffer_from_pool(void) {
    return get_buffer_from_pool_sized(URING_DEFAULT_BUFFER_SIZE);
}

/* Get buffer from pool with specific size */
void *get_buffer_from_pool_sized(size_t size) {
    if (!global_buffer_pool) {
        return allocate_aligned_buffer(size, 4096);
    }

    pthread_mutex_lock(&global_buffer_pool->lock);

    /* Try size-specific pools first */
    void *buffer = get_buffer_from_size_pool(size);
    if (buffer) {
        global_buffer_pool->stats.hits++;
        global_buffer_pool->stats.current_usage++;
        if (global_buffer_pool->stats.current_usage > global_buffer_pool->stats.peak_usage) {
            global_buffer_pool->stats.peak_usage = global_buffer_pool->stats.current_usage;
        }
        pthread_mutex_unlock(&global_buffer_pool->lock);
        return buffer;
    }

    /* Try main pool if size matches */
    if (size <= global_buffer_pool->size) {
        for (int i = 0; i < global_buffer_pool->count; i++) {
            int idx = (global_buffer_pool->next_free + i) % global_buffer_pool->count;
            if (global_buffer_pool->available[idx]) {
                global_buffer_pool->available[idx] = 0;
                global_buffer_pool->next_free = (idx + 1) % global_buffer_pool->count;
                global_buffer_pool->stats.hits++;
                global_buffer_pool->stats.current_usage++;
                if (global_buffer_pool->stats.current_usage > global_buffer_pool->stats.peak_usage) {
                    global_buffer_pool->stats.peak_usage = global_buffer_pool->stats.current_usage;
                }
                pthread_mutex_unlock(&global_buffer_pool->lock);
                return global_buffer_pool->buffers[idx];
            }
        }
    }

    /* No buffer available in pools */
    global_buffer_pool->stats.misses++;
    pthread_mutex_unlock(&global_buffer_pool->lock);

    /* Allocate new aligned buffer */
    buffer = allocate_aligned_buffer(size, global_buffer_pool->alignment);
    if (buffer) {
        pthread_mutex_lock(&global_buffer_pool->lock);
        global_buffer_pool->stats.allocations++;
        global_buffer_pool->stats.total_allocated_bytes += size;
        pthread_mutex_unlock(&global_buffer_pool->lock);
    }

    return buffer;
}

/* Enhanced return buffer to pool */
void return_buffer_to_pool(void *buffer) {
    if (!global_buffer_pool || !buffer) {
        if (buffer) zfree(buffer); /* Use zfree for aligned memory */
        return;
    }

    pthread_mutex_lock(&global_buffer_pool->lock);

    /* Check if buffer belongs to main pool */
    for (int i = 0; i < global_buffer_pool->count; i++) {
        if (global_buffer_pool->buffers[i] == buffer) {
            global_buffer_pool->available[i] = 1;
            global_buffer_pool->stats.current_usage--;
            global_buffer_pool->stats.deallocations++;
            pthread_mutex_unlock(&global_buffer_pool->lock);
            return;
        }
    }

    /* Check if buffer belongs to size-specific pools */
    if (global_buffer_pool->size_pools) {
        for (int i = 0; i < global_buffer_pool->num_size_pools; i++) {
            struct buffer_size_pool *pool = &global_buffer_pool->size_pools[i];
            for (int j = 0; j < pool->count; j++) {
                if (pool->buffers[j] == buffer) {
                    pool->available[j] = 1;
                    global_buffer_pool->stats.current_usage--;
                    global_buffer_pool->stats.deallocations++;
                    pthread_mutex_unlock(&global_buffer_pool->lock);
                    return;
                }
            }
        }
    }

    /* Buffer doesn't belong to any pool, free it */
    global_buffer_pool->stats.deallocations++;
    global_buffer_pool->stats.total_freed_bytes += 0; /* We don't know the size */
    pthread_mutex_unlock(&global_buffer_pool->lock);

    zfree(buffer); /* Use zfree for aligned memory */
}

/* Enhanced cleanup buffer pool */
void cleanup_buffer_pool(void) {
    if (!global_buffer_pool) {
        return;
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "Cleaning up buffer pool - Stats: hits=%lu, misses=%lu, allocations=%lu, peak_usage=%lu",
              (unsigned long)global_buffer_pool->stats.hits,
              (unsigned long)global_buffer_pool->stats.misses,
              (unsigned long)global_buffer_pool->stats.allocations,
              (unsigned long)global_buffer_pool->stats.peak_usage);
    #endif

    /* Clean up main pool buffers */
    if (global_buffer_pool->buffers) {
        for (int i = 0; i < global_buffer_pool->count; i++) {
            if (global_buffer_pool->buffers[i]) {
                zfree(global_buffer_pool->buffers[i]); /* Use zfree for aligned memory */
            }
        }
        zfree(global_buffer_pool->buffers);
    }

    if (global_buffer_pool->available) {
        zfree(global_buffer_pool->available);
    }

    /* Clean up size-specific pools */
    if (global_buffer_pool->size_pools) {
        for (int i = 0; i < global_buffer_pool->num_size_pools; i++) {
            struct buffer_size_pool *pool = &global_buffer_pool->size_pools[i];
            if (pool->buffers) {
                for (int j = 0; j < pool->count; j++) {
                    if (pool->buffers[j]) {
                        zfree(pool->buffers[j]);
                    }
                }
                zfree(pool->buffers);
            }
            if (pool->available) {
                zfree(pool->available);
            }
        }
        zfree(global_buffer_pool->size_pools);
    }

    pthread_mutex_destroy(&global_buffer_pool->lock);
    zfree(global_buffer_pool);
    global_buffer_pool = NULL;
}

/* Get buffer pool statistics */
void get_buffer_pool_stats(char **info) {
    #ifndef REDIS_CLI_BUILD
    if (!global_buffer_pool) {
        *info = sdscatprintf(*info, "buffer_pool_enabled:no\r\n");
        return;
    }

    pthread_mutex_lock(&global_buffer_pool->lock);

    double hit_rate = (global_buffer_pool->stats.hits + global_buffer_pool->stats.misses) > 0 ?
        (double)global_buffer_pool->stats.hits / (global_buffer_pool->stats.hits + global_buffer_pool->stats.misses) * 100.0 : 0.0;

    *info = sdscatprintf(*info,
        "buffer_pool_enabled:yes\r\n"
        "buffer_pool_count:%d\r\n"
        "buffer_pool_size:%d\r\n"
        "buffer_pool_alignment:%d\r\n"
        "buffer_pool_max_size:%d\r\n"
        "buffer_pool_hits:%lu\r\n"
        "buffer_pool_misses:%lu\r\n"
        "buffer_pool_hit_rate:%.2f\r\n"
        "buffer_pool_allocations:%lu\r\n"
        "buffer_pool_deallocations:%lu\r\n"
        "buffer_pool_current_usage:%lu\r\n"
        "buffer_pool_peak_usage:%lu\r\n"
        "buffer_pool_total_allocated_bytes:%lu\r\n"
        "buffer_pool_total_freed_bytes:%lu\r\n"
        "buffer_pool_size_pools:%d\r\n",
        global_buffer_pool->count,
        global_buffer_pool->size,
        global_buffer_pool->alignment,
        global_buffer_pool->max_size,
        (unsigned long)global_buffer_pool->stats.hits,
        (unsigned long)global_buffer_pool->stats.misses,
        hit_rate,
        (unsigned long)global_buffer_pool->stats.allocations,
        (unsigned long)global_buffer_pool->stats.deallocations,
        (unsigned long)global_buffer_pool->stats.current_usage,
        (unsigned long)global_buffer_pool->stats.peak_usage,
        (unsigned long)global_buffer_pool->stats.total_allocated_bytes,
        (unsigned long)global_buffer_pool->stats.total_freed_bytes,
        global_buffer_pool->num_size_pools);

    /* Add size-specific pool stats */
    if (global_buffer_pool->size_pools) {
        for (int i = 0; i < global_buffer_pool->num_size_pools; i++) {
            struct buffer_size_pool *pool = &global_buffer_pool->size_pools[i];
            *info = sdscatprintf(*info,
                "buffer_pool_size_%d_hits:%lu\r\n"
                "buffer_pool_size_%d_misses:%lu\r\n",
                pool->size, (unsigned long)pool->hits,
                pool->size, (unsigned long)pool->misses);
        }
    }

    pthread_mutex_unlock(&global_buffer_pool->lock);
    #endif
}

/* Memory pool optimization functions */

/* Grow buffer pool dynamically */
static int grow_buffer_pool(buffer_pool *pool, int additional_count) {
    if (!pool || additional_count <= 0) {
        return -1;
    }

    int new_count = pool->count + additional_count;
    if (new_count > pool->max_size) {
        new_count = pool->max_size;
        additional_count = new_count - pool->count;
        if (additional_count <= 0) {
            return 0; /* Already at max size */
        }
    }

    /* Reallocate arrays */
    void **new_buffers = zrealloc(pool->buffers, sizeof(void*) * new_count);
    int *new_available = zrealloc(pool->available, sizeof(int) * new_count);

    if (!new_buffers || !new_available) {
        if (new_buffers) zfree(new_buffers);
        if (new_available) zfree(new_available);
        return -1;
    }

    pool->buffers = new_buffers;
    pool->available = new_available;

    /* Allocate new buffers */
    for (int i = pool->count; i < new_count; i++) {
        pool->buffers[i] = allocate_aligned_buffer(pool->size, pool->alignment);
        if (!pool->buffers[i]) {
            /* Allocation failed, stop here */
            new_count = i;
            break;
        }
        pool->available[i] = 1;
        pool->stats.allocations++;
        pool->stats.total_allocated_bytes += pool->size;
    }

    int actually_added = new_count - pool->count;
    pool->count = new_count;
    pool->stats.grows++;

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Buffer pool grown by %d buffers (requested %d), new size: %d",
              actually_added, additional_count, pool->count);
    #endif

    return actually_added;
}

/* Shrink buffer pool to reduce memory usage */
static int shrink_buffer_pool(buffer_pool *pool, int target_count) {
    if (!pool || target_count >= pool->count) {
        return 0;
    }

    int removed_count = 0;

    /* Free buffers from the end, but only if they're available */
    for (int i = pool->count - 1; i >= target_count && i >= 0; i--) {
        if (pool->available[i] && pool->buffers[i]) {
            zfree(pool->buffers[i]);
            pool->buffers[i] = NULL;
            pool->available[i] = 0;
            removed_count++;
            pool->stats.deallocations++;
            pool->stats.total_freed_bytes += pool->size;
        }
    }

    if (removed_count > 0) {
        pool->count = target_count;
        pool->stats.shrinks++;

        /* Reallocate arrays to smaller size */
        pool->buffers = zrealloc(pool->buffers, sizeof(void*) * target_count);
        pool->available = zrealloc(pool->available, sizeof(int) * target_count);

        #ifndef REDIS_CLI_BUILD
        serverLog(LL_DEBUG, "Buffer pool shrunk by %d buffers, new size: %d",
                  removed_count, pool->count);
        #endif
    }

    return removed_count;
}

/* Optimize buffer pool based on usage patterns */
void optimize_buffer_pool(void) {
    if (!global_buffer_pool) {
        return;
    }

    pthread_mutex_lock(&global_buffer_pool->lock);

    /* Calculate usage statistics */
    int available_count = 0;
    for (int i = 0; i < global_buffer_pool->count; i++) {
        if (global_buffer_pool->available[i]) {
            available_count++;
        }
    }

    int used_count = global_buffer_pool->count - available_count;
    double usage_ratio = (double)used_count / global_buffer_pool->count;

    /* Grow pool if usage is high */
    if (usage_ratio > 0.8 && global_buffer_pool->count < global_buffer_pool->max_size) {
        int grow_amount = global_buffer_pool->count / 4; /* Grow by 25% */
        if (grow_amount < 16) grow_amount = 16; /* Minimum growth */
        grow_buffer_pool(global_buffer_pool, grow_amount);
    }
    /* Shrink pool if usage is very low */
    else if (usage_ratio < 0.2 && global_buffer_pool->count > 64) {
        int target_size = used_count * 2; /* Keep 2x current usage */
        if (target_size < 64) target_size = 64; /* Minimum size */
        shrink_buffer_pool(global_buffer_pool, target_size);
    }

    /* Optimize size-specific pools */
    if (global_buffer_pool->size_pools) {
        for (int i = 0; i < global_buffer_pool->num_size_pools; i++) {
            struct buffer_size_pool *pool = &global_buffer_pool->size_pools[i];

            /* Simple optimization: if hit rate is very low, reduce pool size */
            if (pool->hits + pool->misses > 100) {
                double hit_rate = (double)pool->hits / (pool->hits + pool->misses);
                if (hit_rate < 0.1 && pool->count > 8) {
                    /* Reduce this pool size */
                    int target_size = pool->count / 2;
                    if (target_size < 8) target_size = 8;

                    for (int j = pool->count - 1; j >= target_size; j--) {
                        if (pool->available[j] && pool->buffers[j]) {
                            zfree(pool->buffers[j]);
                            pool->buffers[j] = NULL;
                            pool->available[j] = 0;
                        }
                    }
                    pool->count = target_size;
                }
            }
        }
    }

    pthread_mutex_unlock(&global_buffer_pool->lock);
}

/* Prefault memory pages to improve performance */
static void prefault_buffer_memory(void *buffer, size_t size) {
    if (!buffer || size == 0) {
        return;
    }

    /* Touch each page to ensure it's mapped */
    size_t page_size = getpagesize();
    volatile char *ptr = (volatile char*)buffer;

    for (size_t offset = 0; offset < size; offset += page_size) {
        ptr[offset] = ptr[offset]; /* Read and write back */
    }
}

/* Allocate buffer with memory optimization */
void *allocate_optimized_buffer(size_t size) {
    void *buffer = get_buffer_from_pool_sized(size);

    if (buffer) {
        /* Prefault the memory for better performance */
        prefault_buffer_memory(buffer, size);
    }

    return buffer;
}

/* Memory usage monitoring */
void monitor_memory_usage(void) {
    #ifndef REDIS_CLI_BUILD
    if (!global_buffer_pool) {
        return;
    }

    pthread_mutex_lock(&global_buffer_pool->lock);

    size_t total_allocated = global_buffer_pool->stats.total_allocated_bytes;
    size_t total_freed = global_buffer_pool->stats.total_freed_bytes;
    size_t current_usage = total_allocated - total_freed;

    double hit_rate = 0.0;
    if (global_buffer_pool->stats.hits + global_buffer_pool->stats.misses > 0) {
        hit_rate = (double)global_buffer_pool->stats.hits /
                   (global_buffer_pool->stats.hits + global_buffer_pool->stats.misses) * 100.0;
    }

    #ifndef REDIS_CLI_BUILD
    serverLog(LL_DEBUG, "Buffer pool memory usage: current=%zu bytes, hit_rate=%.2f%%, "
              "allocations=%lu, deallocations=%lu, grows=%lu, shrinks=%lu",
              current_usage, hit_rate,
              (unsigned long)global_buffer_pool->stats.allocations,
              (unsigned long)global_buffer_pool->stats.deallocations,
              (unsigned long)global_buffer_pool->stats.grows,
              (unsigned long)global_buffer_pool->stats.shrinks);
    #endif

    pthread_mutex_unlock(&global_buffer_pool->lock);
    #endif
}

#endif /* HAVE_LIBURING */
