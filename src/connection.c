/* ==========================================================================
 * connection.c - connection layer framework
 * --------------------------------------------------------------------------
 * Copyright (C) 2022  zhenwei pi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */

#include "server.h"
#include "connection.h"

static ConnectionType *connTypes[CONN_TYPE_MAX];

int connTypeRegister(ConnectionType *ct) {
    const char *typename = ct->get_type(NULL);
    ConnectionType *tmpct;
    int type;

    /* find an empty slot to store the new connection type */
    for (type = 0; type < CONN_TYPE_MAX; type++) {
        tmpct = connTypes[type];
        if (!tmpct)
            break;

        /* ignore case, we really don't care "tls"/"TLS" */
        if (!strcasecmp(typename, tmpct->get_type(NULL))) {
            serverLog(LL_WARNING, "Connection types %s already registered", typename);
            return C_ERR;
        }
    }

    serverAssert(type < CONN_TYPE_MAX);
    serverLog(LL_VERBOSE, "Connection type %s registered", typename);
    connTypes[type] = ct;

    if (ct->init) {
        ct->init();
    }

    return C_OK;
}

int connTypeInitialize(void) {
    /* currently socket connection type is necessary  */
    serverAssert(RedisRegisterConnectionTypeSocket() == C_OK);

    /* currently unix socket connection type is necessary  */
    serverAssert(RedisRegisterConnectionTypeUnix() == C_OK);

    /* may fail if without BUILD_TLS=yes */
    RedisRegisterConnectionTypeTLS();

    return C_OK;
}

ConnectionType *connectionByType(const char *typename) {
    ConnectionType *ct;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (!ct)
            break;

        if (!strcasecmp(typename, ct->get_type(NULL)))
            return ct;
    }

    serverLog(LL_WARNING, "Missing implement of connection type %s", typename);

    return NULL;
}

/* Cache TCP connection type, query it by string once */
ConnectionType *connectionTypeTcp(void) {
    static ConnectionType *ct_tcp = NULL;

    if (ct_tcp != NULL)
        return ct_tcp;

    ct_tcp = connectionByType(CONN_TYPE_SOCKET);
    serverAssert(ct_tcp != NULL);

    return ct_tcp;
}

/* Cache TLS connection type, query it by string once */
ConnectionType *connectionTypeTls(void) {
    static ConnectionType *ct_tls = NULL;
    static int cached = 0;

    /* Unlike the TCP and Unix connections, the TLS one can be missing
     * So we need the cached pointer to handle NULL correctly too. */
    if (!cached) {
        cached = 1;
        ct_tls = connectionByType(CONN_TYPE_TLS);
    }

    return ct_tls;
}

/* Cache Unix connection type, query it by string once */
ConnectionType *connectionTypeUnix(void) {
    static ConnectionType *ct_unix = NULL;

    if (ct_unix != NULL)
        return ct_unix;

    ct_unix = connectionByType(CONN_TYPE_UNIX);
    return ct_unix;
}

int connectionIndexByType(const char *typename) {
    ConnectionType *ct;

    for (int type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (!ct)
            break;

        if (!strcasecmp(typename, ct->get_type(NULL)))
            return type;
    }

    return -1;
}

void connTypeCleanupAll(void) {
    ConnectionType *ct;
    int type;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (!ct)
            break;

        if (ct->cleanup)
            ct->cleanup();
    }
}

/* walk all the connection types until has pending data */
int connTypeHasPendingData(struct aeEventLoop *el) {
    ConnectionType *ct;
    int type;
    int ret = 0;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && ct->has_pending_data && (ret = ct->has_pending_data(el))) {
            return ret;
        }
    }

    return ret;
}

/* walk all the connection types and process pending data for each connection type */
int connTypeProcessPendingData(struct aeEventLoop *el) {
    ConnectionType *ct;
    int type;
    int ret = 0;

    for (type = 0; type < CONN_TYPE_MAX; type++) {
        ct = connTypes[type];
        if (ct && ct->process_pending_data) {
            ret += ct->process_pending_data(el);
        }
    }

    return ret;
}

sds getListensInfoString(sds info) {
    for (int j = 0; j < CONN_TYPE_MAX; j++) {
        connListener *listener = &server.listeners[j];
        if (listener->ct == NULL)
            continue;

        info = sdscatfmt(info, "listener%i:name=%s", j, listener->ct->get_type(NULL));
        for (int i = 0; i < listener->count; i++) {
            info = sdscatfmt(info, ",bind=%s", listener->bindaddr[i]);
        }

        if (listener->port)
            info = sdscatfmt(info, ",port=%i", listener->port);

        info = sdscatfmt(info, "\r\n");
    }

    return info;
}

/* Week 3: io_uring specific connection functions */
#ifdef HAVE_LIBURING

#include "ae_uring.h"

/* Initialize io_uring specific connection fields */
void connInitUring(connection *conn) {
    if (!conn) return;

    conn->uring_read_buffer = NULL;
    conn->uring_read_size = 0;
    conn->uring_write_buffer = NULL;
    conn->uring_write_size = 0;
    conn->uring_read_pending = 0;
    conn->uring_write_pending = 0;
    conn->uring_read_ctx = NULL;
    conn->uring_write_ctx = NULL;
}

/* Clean up io_uring specific connection fields */
void connCleanupUring(connection *conn) {
    if (!conn) return;

    /* Cancel any pending operations */
    if (conn->uring_read_ctx) {
        /* cancel_operation(conn->el->apidata, conn->uring_read_ctx); */
        conn->uring_read_ctx = NULL;
    }

    if (conn->uring_write_ctx) {
        /* cancel_operation(conn->el->apidata, conn->uring_write_ctx); */
        conn->uring_write_ctx = NULL;
    }

    /* Clean up buffers if they were allocated */
    if (conn->uring_read_buffer) {
        /* aeApiState *state = conn->el->apidata;
        return_buffer_to_pool(state->buffer_pool, conn->uring_read_buffer); */
        zfree(conn->uring_read_buffer);
        conn->uring_read_buffer = NULL;
    }

    if (conn->uring_write_buffer) {
        /* Write buffers are typically not from pool, just clear reference */
        conn->uring_write_buffer = NULL;
    }

    conn->uring_read_size = 0;
    conn->uring_write_size = 0;
    conn->uring_read_pending = 0;
    conn->uring_write_pending = 0;
}

/* Submit io_uring read operation for connection */
int connSubmitUringRead(connection *conn) {
    if (!conn || !conn->el || !conn->el->apidata) {
        return C_ERR;
    }

    aeApiState *state = conn->el->apidata;

    /* Don't submit if already pending */
    if (conn->uring_read_pending) {
        return C_OK;
    }

    /* Create operation context */
    uring_op_context *ctx = zmalloc(sizeof(uring_op_context)); /* create_op_context(conn->fd, URING_OP_READ, AE_READABLE); */
    if (!ctx) {
        return C_ERR;
    }
    memset(ctx, 0, sizeof(uring_op_context));

    /* Get buffer from pool */
    ctx->buffer = zmalloc(16384); /* get_buffer_from_pool(state->buffer_pool); */
    if (!ctx->buffer) {
        /* free_op_context(ctx); */
        zfree(ctx);
        return C_ERR;
    }

    ctx->buffer_size = 16384; /* state->buffer_pool->size; */
    conn->uring_read_ctx = ctx;
    conn->uring_read_pending = 1;

    /* Submit the operation */
    if (/* submit_read_operation(state, conn->fd, ctx) */ -1 < 0) {
        /* return_buffer_to_pool(state->buffer_pool, ctx->buffer); */
        zfree(ctx->buffer);
        /* free_op_context(ctx); */
        zfree(ctx);
        conn->uring_read_ctx = NULL;
        conn->uring_read_pending = 0;
        return C_ERR;
    }

    return C_OK;
}

/* Submit io_uring write operation for connection */
int connSubmitUringWrite(connection *conn, const void *data, size_t len) {
    if (!conn || !conn->el || !conn->el->apidata || !data || len == 0) {
        return C_ERR;
    }

    aeApiState *state = conn->el->apidata;

    /* Don't submit if already pending */
    if (conn->uring_write_pending) {
        return C_ERR;
    }

    /* Create operation context */
    uring_op_context *ctx = zmalloc(sizeof(uring_op_context)); /* create_op_context(conn->fd, URING_OP_WRITE, AE_WRITABLE); */
    if (!ctx) {
        return C_ERR;
    }
    memset(ctx, 0, sizeof(uring_op_context));

    /* Set up write data - note: caller must ensure data remains valid */
    ctx->buffer = (void *)data;
    ctx->buffer_size = len;
    conn->uring_write_ctx = ctx;
    conn->uring_write_pending = 1;
    conn->uring_write_buffer = (void *)data;
    conn->uring_write_size = len;

    /* Submit the operation */
    if (/* submit_write_operation(state, conn->fd, ctx) */ -1 < 0) {
        /* free_op_context(ctx); */
        zfree(ctx);
        conn->uring_write_ctx = NULL;
        conn->uring_write_pending = 0;
        conn->uring_write_buffer = NULL;
        conn->uring_write_size = 0;
        return C_ERR;
    }

    return C_OK;
}

/* Handle io_uring read completion */
void connHandleUringReadCompletion(connection *conn, int result) {
    if (!conn) return;

    conn->uring_read_pending = 0;

    if (result > 0) {
        /* Read successful - data is in the operation context buffer */
        conn->uring_read_size = result;

        /* Call the connection's read handler */
        if (conn->read_handler) {
            conn->read_handler(conn);
        }
    } else if (result == 0) {
        /* Connection closed by peer */
        conn->state = CONN_STATE_CLOSED;
        if (conn->conn_handler) {
            conn->conn_handler(conn);
        }
    } else {
        /* Read error */
        conn->last_errno = -result;
        conn->state = CONN_STATE_ERROR;
        if (conn->conn_handler) {
            conn->conn_handler(conn);
        }
    }

    /* Clear read context */
    conn->uring_read_ctx = NULL;
}

/* Handle io_uring write completion */
void connHandleUringWriteCompletion(connection *conn, int result) {
    if (!conn) return;

    conn->uring_write_pending = 0;

    if (result > 0) {
        /* Write successful */
        conn->uring_write_size = 0;
        conn->uring_write_buffer = NULL;

        /* Call the connection's write handler */
        if (conn->write_handler) {
            conn->write_handler(conn);
        }
    } else {
        /* Write error */
        conn->last_errno = -result;
        conn->state = CONN_STATE_ERROR;
        if (conn->conn_handler) {
            conn->conn_handler(conn);
        }
    }

    /* Clear write context */
    conn->uring_write_ctx = NULL;
}

/* Check if connection has pending io_uring operations */
int connHasUringPendingOps(connection *conn) {
    if (!conn) return 0;
    return conn->uring_read_pending || conn->uring_write_pending;
}

#endif /* HAVE_LIBURING */
