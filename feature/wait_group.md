# Coroutine WaitGroup / Join (Manual add/done)

## Goals
- Provide a coroutine-friendly group wait primitive: `co_await wg.join()`.
- Keep current `spawn` behavior unchanged (still detached fire-and-forget).
- Support cross-thread `add/done` and cross-loop waiters safely.
- Make shutdown deterministic: stop producing work, then wait all children done.

## Non-Goals
- No automatic spawn tracking in this version.
- No blocking thread wait API (`wait()`); only coroutine await.
- No dynamic task ownership transfer model.

## Public API
```cpp
class WaitGroup {
public:
    void add(std::uint64_t n = 1);
    void done();
    bool empty() const noexcept;

    class JoinAwaiter;
    JoinAwaiter join() noexcept;
};
```

## Required Usage Contract
Because `spawn` is unchanged, correctness depends on call-site discipline:

1. Parent must call `wg.add(1)` **before** `spawn(...)`.
2. Child coroutine must call `wg.done()` exactly once before exit.
3. If `spawn(...)` throws after `add(1)`, parent must rollback with `wg.done()`.
4. During shutdown:
   - stop creating new children first,
   - then `co_await wg.join()`.
5. `WaitGroup` destructor requires no outstanding work (`count == 0`).

## Core Invariants
- `count_` is never negative (`done()` on zero is assert).
- Waiter list mutation is protected by `state_mu_`.
- Each waiter resumes at most once.
- Waiter resume is always posted back to waiter's captured `EventLoop`.

## Data Model
```text
WaitGroup
  state_mu_: std::mutex
  count_: uint64_t
  waiters_head_/waiters_tail_: Waiter*

Waiter
  loop: EventLoop*
  handle: coroutine_handle<>
  state: atomic<Waiting|Notified|Resumed|Canceled>
  prev/next: intrusive links
  queued: bool
  notify_entry: EventLoop::NotifyEntry
```

## Join Algorithm
- `await_ready()`:
  - returns true when `count_ == 0`.
- `await_suspend()`:
  - captures current loop,
  - allocates waiter and enqueues if still `count_ > 0`,
  - if already zero, do not suspend.
- `await_resume()`:
  - no return value.
- awaiter destructor:
  - if still waiting, cancel waiter from queue.

## Completion Algorithm
- `done()` decrements `count_`.
- If new value is non-zero: return.
- On transition `1 -> 0`:
  - detach the whole waiter list under lock,
  - mark waiters `Notified`,
  - post resume callbacks to each waiter loop.

## Race Handling
- `join` cancellation vs `done` notification:
  - waiter state machine prevents double resume.
  - `Notified -> Canceled` clears handle; posted callback becomes no-op.
- `add`/`done` can happen on any thread.
- Waiter queue operations are lock-protected.

## Failure Semantics
- `done()` on empty group triggers assert (usage bug).
- `add()` overflow triggers assert.
- Destroying `WaitGroup` with outstanding count or waiters triggers assert.

## Suggested Tests
1. `join()` returns immediately when count is zero.
2. single `add/done` resumes one waiter.
3. multiple waiters all resume when count reaches zero.
4. multiple loops wait on same group and resume on correct loop.
5. `done()` underflow assertion.

