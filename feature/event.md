# Event Loop Design (Linux)

## Goals
- Linux-only implementation based on `epoll` + `eventfd`.
- Multi-producer, single-consumer task queue (MPSC) for cross-thread scheduling.
- Strictly asynchronous execution on the loop thread (no inline execution during `post`).
- Timers integrated into a single event loop.
- Defer entries use fixed callbacks with optional cancel handling.

## Threading Model
- One event loop thread owns the poller and internal state.
- Any thread may call `post(NotifyEntry&)`, `cancel(NotifyEntry&)`, `post_at`, `cancel`, or `stop`.
- Cross-thread calls enqueue into the defer MPSC and signal `eventfd` to wake the loop.

## Loop Group
`EventLoopGroup` owns a fixed set of loops and a `ThreadGroup`. `start()` runs one
loop per thread, and `post()` schedules to the current loop when called from a
loop thread (otherwise it uses round-robin selection).

## Wakeup Strategy
- `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)` is registered in `epoll`.
- Producers write `uint64_t(1)` to `eventfd` to wake the loop.
- The loop drains `eventfd` before processing queued commands.
- An atomic flag (`wakeup_pending_`) prevents redundant wakeups.

## Defer Queue
`NotifyEntry` provides an intrusive, fixed-callback scheduling primitive:
- `post(NotifyEntry&)` enqueues the entry for execution on the loop thread.
- `cancel(NotifyEntry&)` marks the entry canceled; the loop executes `on_cancel` when drained.
- Each entry has two callbacks: `on_run` and `on_cancel` (either may be null).
- Cancellation is best-effort if it races with draining.

## Local Queue vs MPSC Queue
- Local queue is loop-thread only, intrusive, and supports O(1) erase for cancel.
- Local entries may reference awaiter memory and coroutine handles.
- MPSC queue is cross-thread, heap-allocated nodes, and does not support erase.
- MPSC tasks must not capture awaiter pointers directly; they carry `SharedState`
  or a cancel flag and self-discard on dequeue if canceled.
- Cross-thread tasks should enqueue a loop-local entry when they need cancelable
  behavior or awaiter access.

## Awaiter Timeout + Cancellation Safety (Design Rules)
- Timeouts are driven by the owning `EventLoop` timer; the timeout callback and
  the awaited completion both execute on the same loop thread.
- Timeout callbacks may resume coroutines inline (no local-queue hop), as long
  as the local queue is cancelable and the guarantees below are upheld.
- Any callback that touches awaiter memory (awaiter pointer, coroutine handle,
  stack storage) must be loop-local and cancelable. Prefer a loop-only intrusive
  queue (O(1) erase) for these tasks.
- Cross-thread scheduling uses the MPSC queue with heap-allocated nodes. MPSC
  nodes must not capture awaiter pointers directly; they carry a `SharedState`
  or cancel flag and drop themselves on dequeue if canceled.
- EventLoop guarantees required to avoid a separate resume-state gate:
  - Local queue is "pop-and-run": tasks are removed only immediately before
    execution (no prefetch/temporary staging).
  - `cancel_local` is synchronous on the loop thread and removes the node if it
    is still queued; `false` means it has already run.
  - Normal completion is posted only through the local queue (not inline), so
    timeout and normal completion cannot both run inline.
  - Cancellation cuts off the event source (poller/watch/timer) so no new
    completion task is enqueued after cancel.

## TimerQueue (Heap)
`TimerQueue` is a C++ translation of `libuv`'s `heap-inl.h`, used as an intrusive
min-heap for timer nodes. It provides heap primitives and does not own timer data.

```cpp
class TimerQueue {
public:
    struct Node {
        Node *left;
        Node *right;
        Node *parent;
    };

    void init();
    Node *min() const;
    std::size_t size() const;
    bool empty() const;

    void insert(Node *node);
    void remove(Node *node);
    void dequeue();
};
```

## API Summary
```cpp
namespace fiber::event {

class EventLoop : public fiber::async::IScheduler {
public:
    void run();
    void run_once();
    void stop();

    void post(NotifyEntry &entry);
    void cancel(NotifyEntry &entry);

    void post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry);
    void cancel(TimerEntry &entry);
};

class EventLoopGroup : public fiber::async::IScheduler {
public:
    explicit EventLoopGroup(std::size_t size);

    void start();
    void stop();
    void join();

    EventLoop &at(std::size_t index);

};

} // namespace fiber::event
```

## Loop Cycle
1) Drain defer queue and execute due callbacks.
2) Execute due timers.
3) `epoll_wait` with timeout from the next deadline.
4) Dispatch IO callbacks.

## File Layout
- `src/event/EventLoop.h|.cpp`
- `src/event/Poller.h|.cpp`
- `src/event/TimerQueue.h|.cpp` (libuv heap translation)
- `src/event/MpscQueue.h` (header-only)
- `src/async/Scheduler.h`
- `src/async/Coroutine.h|.cpp`

## Implementation Notes
- All loop-side execution is strictly async (no inline execution in `post`).
- The poller and timer queue are loop-thread only.
