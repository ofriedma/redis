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

/* Forward declarations for buffer pool functions */
int init_buffer_pool(int count, int size);
void *get_buffer_from_pool(void);
void return_buffer_to_pool(void *buffer);
void cleanup_buffer_pool(void);

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
    
    /* For now, skip buffer ring setup as it requires newer liburing
     * We'll use regular buffer allocation instead */
    #ifndef REDIS_CLI_BUILD
    serverLog(LL_NOTICE, "Buffer ring setup skipped - using regular buffer allocation");
    #endif

    /* Clean up allocated memory since we're not using buffer rings */
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

/* Get buffer from ring */
void *get_buffer_from_ring(aeApiState *state, int *buffer_id) {
    if (!state->buffer_ring_enabled) {
        *buffer_id = -1;
        return get_buffer_from_pool();
    }

    /* Since we're not using actual buffer rings, use buffer pool */
    *buffer_id = -1;
    return get_buffer_from_pool();
}

/* Return buffer to ring */
void return_buffer_to_ring(aeApiState *state, int buffer_id) {
    /* Since we're not using actual buffer rings, this is a no-op for buffer_id >= 0 */
    (void)state;
    (void)buffer_id;
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

/* Pre-allocate buffer pool for non-ring operations */
typedef struct buffer_pool {
    void **buffers;
    int *available;
    int count;
    int size;
    int next_free;
    pthread_mutex_t lock;
} buffer_pool;

static buffer_pool *global_buffer_pool = NULL;

/* Initialize buffer pool */
int init_buffer_pool(int count, int size) {
    if (global_buffer_pool) {
        return 0; /* Already initialized */
    }
    
    global_buffer_pool = zmalloc(sizeof(buffer_pool));
    if (!global_buffer_pool) {
        return -1;
    }
    
    global_buffer_pool->buffers = zmalloc(sizeof(void*) * count);
    global_buffer_pool->available = zmalloc(sizeof(int) * count);
    
    if (!global_buffer_pool->buffers || !global_buffer_pool->available) {
        cleanup_buffer_pool();
        return -1;
    }
    
    /* Allocate buffers */
    for (int i = 0; i < count; i++) {
        global_buffer_pool->buffers[i] = zmalloc(size);
        if (!global_buffer_pool->buffers[i]) {
            /* Clean up allocated buffers */
            for (int j = 0; j < i; j++) {
                zfree(global_buffer_pool->buffers[j]);
            }
            cleanup_buffer_pool();
            return -1;
        }
        global_buffer_pool->available[i] = 1;
    }
    
    global_buffer_pool->count = count;
    global_buffer_pool->size = size;
    global_buffer_pool->next_free = 0;
    pthread_mutex_init(&global_buffer_pool->lock, NULL);
    
    return 0;
}

/* Get buffer from pool */
void *get_buffer_from_pool(void) {
    if (!global_buffer_pool) {
        return zmalloc(URING_DEFAULT_BUFFER_SIZE);
    }
    
    pthread_mutex_lock(&global_buffer_pool->lock);
    
    /* Find available buffer */
    for (int i = 0; i < global_buffer_pool->count; i++) {
        int idx = (global_buffer_pool->next_free + i) % global_buffer_pool->count;
        if (global_buffer_pool->available[idx]) {
            global_buffer_pool->available[idx] = 0;
            global_buffer_pool->next_free = (idx + 1) % global_buffer_pool->count;
            pthread_mutex_unlock(&global_buffer_pool->lock);
            return global_buffer_pool->buffers[idx];
        }
    }
    
    pthread_mutex_unlock(&global_buffer_pool->lock);
    
    /* No buffer available, allocate new one */
    return zmalloc(global_buffer_pool->size);
}

/* Return buffer to pool */
void return_buffer_to_pool(void *buffer) {
    if (!global_buffer_pool || !buffer) {
        zfree(buffer);
        return;
    }
    
    pthread_mutex_lock(&global_buffer_pool->lock);
    
    /* Check if buffer belongs to pool */
    for (int i = 0; i < global_buffer_pool->count; i++) {
        if (global_buffer_pool->buffers[i] == buffer) {
            global_buffer_pool->available[i] = 1;
            pthread_mutex_unlock(&global_buffer_pool->lock);
            return;
        }
    }
    
    pthread_mutex_unlock(&global_buffer_pool->lock);
    
    /* Buffer doesn't belong to pool, free it */
    zfree(buffer);
}

/* Cleanup buffer pool */
void cleanup_buffer_pool(void) {
    if (!global_buffer_pool) {
        return;
    }
    
    if (global_buffer_pool->buffers) {
        for (int i = 0; i < global_buffer_pool->count; i++) {
            if (global_buffer_pool->buffers[i]) {
                zfree(global_buffer_pool->buffers[i]);
            }
        }
        zfree(global_buffer_pool->buffers);
    }
    
    if (global_buffer_pool->available) {
        zfree(global_buffer_pool->available);
    }
    
    pthread_mutex_destroy(&global_buffer_pool->lock);
    zfree(global_buffer_pool);
    global_buffer_pool = NULL;
}

#endif /* HAVE_LIBURING */
