# Examples

Demo applications for aleff (greenlet-based algebraic effects).

All demos follow the same principle: **business logic only invokes effects, and behavior changes by swapping handlers.**

## How to run

```sh
uv run python examples/<filename>.py
```

## Demos

### demo_di.py — Dependency Injection

A user registration service that separates business logic from side effects (DB, email, logging).

- **In-memory handler**: uses a dict as DB for testing
- **Async handler**: simulates external services (DB/SMTP) with async operations
- **Abort handler**: returns without resume to short-circuit the entire computation

### demo_record_replay.py — Record / Replay

Record effect results and replay the same business logic without side effects.

- **Record handler**: calls a simulated external API and records each effect's name, arguments, and result in a JSON-serializable format
- **Replay handler**: reads recorded results sequentially and re-executes without external calls, verifying effect name and argument consistency

Use cases: test reproduction, incident debugging, caching expensive external calls.

### demo_transaction.py — Transactions

A payroll processing example that changes transaction semantics by swapping handlers.

- **Auto-commit handler**: applies each operation to DB immediately
- **Transactional handler**: buffers operations, commits on success, rolls back on failure
- **Transactional + validation handler**: validates balance on each transfer, aborts (no resume) the entire transaction on overdraft

### demo_autodiff.py — Automatic Differentiation (forward-mode)

Arithmetic primitives (add, mul, sin, exp, ...) defined as effects. Same math expressions, different handlers.

- **Evaluate handler**: normal computation with `float`
- **Differentiate handler**: propagates value and derivative simultaneously with Dual numbers (`val + dot * epsilon`)

The math expressions don't know what "numbers" are — the handler decides by passing `float` or `Dual`.

### demo_backprop.py — Automatic Differentiation (reverse-mode / backpropagation)

Reverse-mode AD on the same math expressions and effects as the forward-mode demo, **without building an explicit tape**.

- **Backprop handler**: after `k(result)` resumes (= all remaining forward computation completes), executes the backward pass. The handler call stack itself serves as the computation graph, eliminating the need for explicit tape construction.

Comparison with forward-mode AD:
- forward-mode: each handler propagates `k(Dual(...))` with value and derivative simultaneously. Best for single-variable differentiation.
- reverse-mode: forward/backward separated by code before/after `k()`. Best for multi-variable/multi-parameter differentiation (one backward pass yields all gradients).

### demo_shallow_state.py — State Machine (shallow handlers)

Implements mutable state (`get`/`put`) and a traffic light controller using **shallow handlers**.

- **State effect**: each `get`/`put` is handled once, then the handler re-installs itself with the (potentially updated) state value — state is encoded in the handler, not in mutable variables
- **Deep handler comparison**: the same state effect implemented with a deep handler and a mutable variable, showing the two approaches side by side
- **Traffic light**: RED → GREEN → YELLOW → RED state transitions, where each `next_signal()` is handled by a shallow handler that re-installs the next state

### demo_shift_reset.py — shift/reset and shift0/reset0

Delimited continuations implemented via algebraic effect handlers, demonstrating the correspondence:

- **shift/reset** = deep handler (handler stays installed across `k`)
- **shift0/reset0** = shallow handler (handler removed before `k` resumes)

Examples:
- `k(k(10))` with deep handler: both `k` calls run under the same delimiter
- Multiple shifts with shallow handler: each `k(v)` must be wrapped in `reset0()` to re-delimit
- **Abort**: discarding the continuation (both deep and shallow)
- **Generator**: `yield` via `shift0`/`reset0` — the consumer calls `k` inside a fresh `reset0` to pull the next value
