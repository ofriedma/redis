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

### 7. Code-Level Changes and Examples

> **Note:** In a real io_uring-based implementation, the async I/O API must have access to the io_uring ring context (struct io_uring). This can be achieved by either passing a pointer to the ring as an argument to async functions, or by storing the ring in the event loop struct (e.g., aeEventLoop) and accessing it internally. The simplified API signatures in this document are for illustration; actual implementations must ensure the ring context is available for all async operations.

#### Event Loop API Changes
- The event loop (see `src/ae.c`, `src/ae.h`) will need new APIs for submitting async I/O operations:
  - `aeAsyncRead(fd, buffer, len, callback, user_data)`
  - `aeAsyncWrite(fd, buffer, len, callback, user_data)`
  - `aeAsyncAccept(fd, callback, user_data)`
- The event loop must track outstanding I/O requests and dispatch completions to the correct callback.
- Example (pseudo-C):
  ```c
  // New API for async read
  int aeAsyncRead(int fd, void *buf, size_t len, void (*cb)(int fd, ssize_t nread, void *buf, void *user_data), void *user_data);
  ```

#### Handler Refactoring
- Handlers such as `readQueryFromClient` (in `src/networking.c`) must be split into:
  - A function to submit the async read (e.g., `submitReadQueryFromClient`)
  - A completion callback (e.g., `readQueryFromClientAsync`)
- Example (pseudo-C):
  ```c
  // Called when a new client is ready for input
  void submitReadQueryFromClient(client *c) {
      aeAsyncRead(c->fd, c->querybuf, sizeof(c->querybuf), readQueryFromClientAsync, c);
  }

  // Completion callback
  void readQueryFromClientAsync(int fd, ssize_t nread, void *buf, void *user_data) {
      client *c = (client*)user_data;
      if (nread > 0) {
          // Process buffer, parse commands, update state
      } else if (nread == 0) {
          // Client closed connection
      } else {
          // Handle error
      }
      // If more data needed:
      // submitReadQueryFromClient(c);
  }
  ```
- All direct calls to `read()`/`write()` in handlers must be replaced with async submission and completion logic.

#### Buffer Management
- Buffers may be managed per-client or via a central pool.
- The event loop or a buffer manager should allocate and recycle buffers for async I/O.
- Example:
  ```c
  void *buf = buffer_pool_get();
  aeAsyncRead(fd, buf, BUFSIZE, callback, user_data);
  // In callback: buffer_pool_release(buf);
  ```

#### Affected Functions (Non-Exhaustive)
- `readQueryFromClient` (src/networking.c): Refactor to async pattern.
- `writeToClient` (src/networking.c): Refactor to async write.
- `acceptTcpHandler` (src/networking.c): Refactor to async accept.
- AOF and replication I/O (src/aof.c, src/replication.c): Refactor to async file/socket I/O.
- Event loop API and implementation (src/ae.h, src/ae.c): Add async I/O support and completion dispatch.

#### Overview of Changes
- Add async I/O submission and completion APIs to the event loop.
- Refactor networking and I/O handlers to use async callbacks.
- Implement buffer management for async operations.
- Update error handling to propagate I/O errors via callbacks.
- Gradually migrate all blocking I/O to async equivalents.

## Implementation Steps

The following steps outline a recommended path for refactoring Redis to support true async I/O. Each step is designed to be as self-contained as possible, allowing for incremental progress and easier review/testing.

1. **Introduce Async I/O Abstractions**
   - Define async I/O API functions in the event loop (e.g., `aeAsyncRead`, `aeAsyncWrite`, `aeAsyncAccept`).
   - Add callback and user data support for completions.
   - Integrate the io_uring ring context into the event loop struct.

2. **Implement Buffer Management**
   - Create a buffer pool or per-client buffer management system for async operations.
   - Ensure buffers can be efficiently allocated, reused, and released.

3. **Add Async I/O Submission and Completion Logic**
   - Implement logic in the event loop to submit I/O requests to io_uring and dispatch completions to registered callbacks.
   - Add tracking for outstanding requests and their associated state.

4. **Refactor Client Read Path**
   - Refactor `readQueryFromClient` to use async read submission and a completion callback (e.g., `readQueryFromClientAsync`).
   - Update connection management code to submit the initial async read for each client.

5. **Refactor Client Write Path**
   - Refactor `writeToClient` to use async write submission and a completion callback.
   - Update all code paths that trigger client writes to use the new async API.

6. **Refactor Accept Path**
   - Refactor `acceptTcpHandler` to use async accept operations and completion callbacks.
   - Update server socket setup to submit async accept requests.

7. **Refactor AOF and Replication I/O**
   - Update AOF (Append Only File) and replication code to use async file and socket I/O APIs.
   - Ensure all blocking I/O in these subsystems is replaced with async equivalents.

8. **Update Error Handling and State Management**
   - Ensure all async handlers properly handle partial completions, errors, and cancellations.
   - Refactor state management in handlers to support async operation lifecycles.

9. **Testing and Validation**
   - Add and update tests to cover async I/O paths, including edge cases and error scenarios.
   - Validate performance and correctness under high concurrency.

10. **Deprecate and Remove Blocking I/O Paths**
    - Once all major I/O paths are async, remove legacy blocking I/O code and readiness-based event loop logic.
    - Update documentation to reflect the new async architecture.

---

*Last updated: July 14, 2025*
