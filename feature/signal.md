# Signal Await Design (Linux signalfd)

## Goals
- Provide coroutine-friendly signal waiting on Linux.
- Use `signalfd` + `epoll` to integrate with the event loop.
- Resume waiting coroutines on the owning `EventLoop` thread.
- Best-effort FIFO fairness per signal.
- Cancellation-safe: waiter destruction cancels pending waits.

## Non-Goals
- No POSIX `sigwaitinfo` dispatcher thread in the baseline design.
- No strict fairness guarantees across signals.
- No timed wait yet (future generic timeout semantics can be layered).

## Threading Model
- All handled signals are **blocked** in all threads via `pthread_sigmask`.
- `signalfd` is registered in the loop `epoll` set; signal delivery happens on
  the loop thread via the poller callback.
- All waiter queues and pending queues are owned and mutated by the loop thread.

## Core Components

### SignalService (event layer)
Owns `signalfd`, pending queues, and waiter queues. Binds to a single
`EventLoop` instance.

Responsibilities:
- Install signal mask (outside or at attach).
- Create/register `signalfd` with the loop poller.
- Receive deliveries on loop thread and notify waiters.
- Manage cancellation and shutdown.

### SignalAwaiter (async layer)
Coroutine awaiter for a single signal.
- Checks `pending` first.
- Enqueues a waiter and suspends.
- Resumes on loop thread with `SignalInfo`.

## Data Structures
- `WaiterQueue[NSIG]`: FIFO for coroutines waiting on each signal.
- `PendingQueue[NSIG]`: FIFO of `SignalInfo` for signals received before a waiter.
- `Waiter` state machine: `Waiting -> Notified -> Resumed`, or `Waiting -> Canceled`.

## Delivery Flow
1) `signalfd` becomes readable on the loop `epoll` callback.
2) Loop thread drains `signalfd` into `signalfd_siginfo` records.
3) Loop thread dispatches:
   - If there is a waiter: pop FIFO waiter, mark `Notified`, store info,
     then post waiter resume via defer.
   - Else: push `SignalInfo` into `pending`.

## Cancellation Races
- `SignalAwaiter` destructor cancels if still `Waiting`.
- If `Notified`, mark `Canceled` so resume callback becomes a no-op.
- All queue operations happen on the loop thread; no external locks needed.

## Interface Skeleton (Aligned with Current Layout)
```cpp
// src/async/Signal.h
namespace fiber::async {

struct SignalInfo {
    int signum{};
    int code{};
    pid_t pid{};
    uid_t uid{};
    int status{};
    int errno_{};
    std::intptr_t value{};
};

class SignalSet {
public:
    SignalSet();
    SignalSet &add(int signum);
    SignalSet &remove(int signum);
    bool contains(int signum) const noexcept;
    const sigset_t &native() const noexcept;
private:
    sigset_t set_{};
};

class SignalAwaiter {
public:
    explicit SignalAwaiter(int signum);
    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    SignalInfo await_resume() noexcept;
private:
    int signum_{};
    // waiter is heap-allocated while suspended
};

SignalAwaiter wait_signal(int signum);

} // namespace fiber::async
```

```cpp
// src/event/SignalService.h
namespace fiber::event {

class SignalService {
public:
    explicit SignalService(EventLoop &loop);
    ~SignalService();

    // Attach installs signalfd and must run on the loop thread.
    bool attach(const fiber::async::SignalSet &mask);
    void detach();

    static SignalService &current();
    static SignalService *current_or_null() noexcept;

    // Loop-thread only.
    void enqueue_waiter(int signum, fiber::async::detail::SignalWaiter *waiter);
    void cancel_waiter(fiber::async::detail::SignalWaiter *waiter);
    bool try_pop_pending(int signum, fiber::async::SignalInfo &out);

private:
    struct SignalItem : Poller::Item {
        SignalService *service = nullptr;
    };

    struct WaiterQueue {
        fiber::async::detail::SignalWaiter *head = nullptr;
        fiber::async::detail::SignalWaiter *tail = nullptr;
    };

    void on_delivery(const fiber::async::SignalInfo &info);
    void drain_signalfd();
    static void on_signalfd(Poller::Item *item, int fd, IoEvent events);

    EventLoop &loop_;
    fiber::async::SignalSet mask_{};
    int signalfd_ = -1;

    // loop-thread only state:
    // WaiterQueue waiters_[NSIG];
    // PendingQueue pending_[NSIG];
};

} // namespace fiber::event
```

### Waiter Shape (Matches EventLoop::NotifyEntry Pattern)
```cpp
// src/async/Signal.h (internal detail)
struct SignalWaiter {
    fiber::event::EventLoop::NotifyEntry notify_entry{};
    std::coroutine_handle<> handle{};
    fiber::event::EventLoop *loop = nullptr;
    std::atomic<State> state{State::Waiting};
    SignalInfo info{};
    SignalWaiter *prev = nullptr;
    SignalWaiter *next = nullptr;
    bool queued = false;
    int signum = 0;

    static void on_run(SignalWaiter *self);
};
```

The loop thread resumes a waiter by:
```
loop->post<SignalWaiter, &SignalWaiter::notify_entry,
           &SignalWaiter::on_run>(*waiter);
```

## Suggested File Layout
- `src/async/Signal.h|.cpp` (awaiter + public API)
- `src/event/SignalService.h|.cpp` (signalfd + queues)
- `feature/signal.md` (this doc)

## Test Notes (GoogleTest + CTest)
Suggested tests live under `tests/SignalTest.cpp` and are wired to
`fiber_tests` in `CMakeLists.txt`.

1) **Single waiter receives signal**
   - Block `SIGUSR1` in the test thread.
   - Create `EventLoop`, `SignalService`, `attach` with mask {SIGUSR1}.
   - `spawn(loop, [&] { auto info = co_await wait_signal(SIGUSR1); ... });`
   - Raise `SIGUSR1` via `kill(getpid(), SIGUSR1)`.
   - Run loop until waiter resumes; assert `info.signum == SIGUSR1`.

2) **Pending delivery before await**
   - Send `SIGUSR1` before starting the awaiter.
   - `co_await wait_signal(SIGUSR1)` should be ready immediately.

3) **FIFO fairness for multiple waiters**
   - Queue two awaiters for `SIGUSR1`.
   - Send two signals; assert resume order matches enqueue order.

4) **Cancellation**
   - Start awaiter and destroy it before signal arrives.
   - Send signal, ensure it goes to next waiter or pending queue.

Notes:
- All signal tests must block the handled signals in the test thread to prevent
  default signal delivery.
- Avoid `SIGALRM`/`SIGCHLD` to reduce interference with the test runner.

## Signal Masking Strategy
- `EventLoopGroup::start(mask)` blocks the mask on each worker thread before
  `EventLoop::run()` (so callers don't need to do it manually).
- The test/main thread should also block the same mask if it raises signals.
- The loop thread uses the same mask and receives signals via `signalfd`.

## Shutdown Semantics
- `detach()` closes `signalfd`; call it when no active waiters remain.
- Pending signals are dropped on shutdown.

## Extensions (Future)
- Support "wait any" (multi-signal wait) using a shared token to avoid
  double-resume across multiple queues.
- Add POSIX `sigwaitinfo` backend for non-Linux targets with the same API.
