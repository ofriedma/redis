# Refactoring Redis for True Async I/O Support

## Overview

This document outlines the architectural and code changes required to refactor the Redis event loop and event handler model to support true asynchronous I/O (async I/O), as enabled by modern Linux interfaces such as io_uring. The goal is to move away from the current readiness-based, blocking handler model to a completion-based, non-blocking async I/O model.

## Motivation

- **Performance**: Reduce latency and increase throughput by eliminating blocking I/O in handlers.
- **Scalability**: Better utilize system resources and handle more concurrent connections.
- **Modernization**: Enable Redis to leverage advanced kernel features (e.g., io_uring, AIO).

## Current Model

- The event loop notifies handlers when a file descriptor is readable or writable.
- Handlers perform blocking read/write operations directly on the socket or file descriptor.
- The event loop is single-threaded and synchronous.

## Target Async I/O Model

- The event loop submits async read/write operations to the kernel (e.g., via io_uring).
- Handlers are invoked only when I/O operations complete, not when the FD is merely ready.
- Handlers process data buffers provided by the kernel, not by reading/writing directly.
- The event loop manages outstanding I/O requests and their completion callbacks.

## Required Changes

### 1. Event Loop API
- Redesign the event loop API to support submission of async I/O operations (read, write, accept, etc.).
- Add support for registering completion callbacks for each I/O operation.
- Track outstanding I/O requests and their associated state.

### 2. Event Handler Model
- Refactor handlers to be completion-based: they receive data buffers or notification of write completion, not readiness.
- Handlers must be non-blocking and should not perform direct I/O.
- State management: Handlers may need to maintain state across multiple async operations (e.g., partial reads/writes).

### 3. Buffer Management
- The event loop or a buffer manager must allocate and manage buffers for async I/O.
- Handlers should process data in provided buffers and return them for reuse.
- Consider zero-copy techniques where possible.

### 4. Error Handling
- Handlers must be able to handle partial completions, errors, and cancellations.
- The event loop should propagate I/O errors to the appropriate handler.

### 5. Backward Compatibility
- Provide a migration path: support both readiness-based and completion-based handlers during transition.
- Gradually refactor core networking and AOF code to use async I/O.

### 6. Testing and Debugging
- Add tests for async I/O paths, including edge cases (partial, out-of-order, and failed completions).
- Provide debugging tools for tracking outstanding I/O and handler state.

## Example: Async Read Flow for `readQueryFromClient`

### Current Flow
1. The event loop notifies `readQueryFromClient` when the client socket is readable.
2. `readQueryFromClient` performs a blocking read from the socket into the client's query buffer.
3. The handler processes the buffer and parses commands.

### Target Async I/O Flow
1. When a new client connection is established, the connection management code (not the event loop itself) submits the initial async read operation for the client socket, providing a buffer and a completion callback (e.g., `readQueryFromClientAsync`).
2. On I/O completion, the event loop invokes the `readQueryFromClientAsync` handler with the buffer and result (number of bytes read, or error).
3. `readQueryFromClientAsync` processes the data in the buffer, updates the client's state, and parses commands. If more data is needed, it can submit another async read by requesting it from the event loop.
4. The event loop is responsible only for dispatching completions, not for initiating application-specific I/O.
5. The handler never blocks and does not perform direct I/O; all reads are managed by the async I/O subsystem.

### Key Changes
- The initial async read is submitted by the connection management logic, not the event loop itself.
- The handler is invoked on I/O completion, not readiness.
- Buffer management is handled by the event loop or a buffer manager.
- The handler must be able to process partial reads and maintain state across multiple async operations.

## Challenges

- Refactoring stateful handlers to work with callbacks and async completions.
- Managing buffer lifetimes and memory efficiently.
- Ensuring correctness and performance under high concurrency.

## References

- [io_uring documentation](https://kernel.dk/io_uring.pdf)
- [libuv async I/O architecture](https://libuv.org/)
- [Redis event loop design](https://github.com/redis/redis/blob/unstable/src/ae.c)

---

*Last updated: July 14, 2025*
