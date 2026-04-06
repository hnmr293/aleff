# aleff

Algebraic effects for Python — deep, stateful, multi-shot handlers via greenlet-based delimited continuations.

## Features

- **Deep handlers** — effects propagate through nested function calls without annotation
- **Stateful handlers** — handler functions can execute code after `resume`, enabling patterns like transactions and reverse-mode AD
- **Multi-shot continuations** — `resume` can be called multiple times in a single handler, enabling backtracking search, non-determinism, and other advanced patterns
- **Sync and async** — both synchronous (`Handler`) and asynchronous (`AsyncHandler`) handlers are supported, with transparent bridging between the two
- **Effect composition** — `@effect(step1, step2)` collects effect sets transitively from decorated functions
- **Introspection** — `effects(fn)` and `unhandled_effects(fn, h)` for querying and validating effect coverage
- **Typed** — effect parameters and return types are checked by type checkers (pyright, ty)
- **No macros, no code generation** — pure Python library built on [greenlet](https://github.com/python-greenlet/greenlet) and a small CPython C extension

## Requirements

- CPython 3.12, 3.13, or 3.14 (CPython-specific C extension)
- greenlet >= 3.3.2
- Linux / macOS (Windows not yet supported — see [#1](https://github.com/hnmr293/aleff/issues/1))
- **Note**: Free-threaded (no-GIL) builds are not supported — see [#3](https://github.com/hnmr293/aleff/issues/3)

## Installation

From source:

```sh
git clone https://github.com/hnmr293/aleff.git
cd aleff
pip install .
```

## Quick start

```python
from aleff import effect, Effect, Resume, create_handler

# Define effects
read: Effect[[], str] = effect("read")
write: Effect[[str], int] = effect("write")

# Write business logic using effects
def run():
    s = read()
    return write(s)

# Provide handler implementations
h = create_handler(read, write)

@h.on(read)
def _read(k: Resume[str, int]):
    return k("file contents")

@h.on(write)
def _write(k: Resume[int, int], contents: str):
    print(f"writing: {contents}")
    return k(len(contents))

result = h(run)
print(result)  # 13
```

### Multi-shot example

```python
from aleff import effect, Effect, Handler, Resume, create_handler

choose: Effect[[int], int] = effect("choose")

h: Handler[list[int]] = create_handler(choose)

@h.on(choose)
def _choose(k: Resume[int, list[int]], n: int):
    # Resume once for each choice and collect all results
    results: list[int] = []
    for i in range(n):
        results += k(i)
    return results

def computation():
    x = choose(3)  # 0, 1, or 2
    y = choose(2)  # 0 or 1
    return [x * 10 + y]

result = h(computation)
print(result)  # [0, 1, 10, 11, 20, 21]
```

## How it works

Effects are declared as typed values and invoked like regular function calls. A `Handler` intercepts these calls via greenlet-based context switching:

1. Business logic runs in a greenlet
2. When an effect is invoked, control switches to the handler
3. The handler processes the effect and calls `resume(value)` to return a value
4. If `resume` is called multiple times, each call restores a snapshot of the continuation's frames (multi-shot)
5. If the handler returns without calling `resume`, the computation is **aborted** (early exit)

Because handlers use greenlets (not exceptions), the control flow is:
- **Transparent** — no `yield`, `await`, or special syntax in business logic
- **Stateful** — code after `resume` runs after the rest of the computation completes, enabling reverse-order execution (useful for backpropagation, transactions, etc.)

Multi-shot continuations are implemented via a CPython C extension (`aleff._multishot.v1._aleff`) that snapshots and restores interpreter frame chains.

### Package structure

| Package | Description |
|---|---|
| `aleff` | Default: re-exports `aleff.multishot` (multi-shot handlers) |
| `aleff.multishot` | Multi-shot handlers with frame snapshot/restore |
| `aleff.oneshot` | One-shot handlers (no C extension required) |

## Examples

See [`examples/`](examples/) for demonstrations:

- **N-Queens** — backtracking search via multi-shot continuations
- **Amb / Logic puzzle** — Scheme-style `amb` operator and constraint solving (SICP Exercise 4.42)
- **Probability** — exact discrete probability distributions via weighted multi-shot
- **Dependency injection** — swap DB/email/logging implementations
- **Record/Replay** — record effect results, replay without side effects
- **Transactions** — buffer writes, commit on success, rollback on failure
- **Automatic differentiation** — forward-mode (dual numbers) and reverse-mode (backpropagation) with the same math expressions

## API reference

| Function / Class | Description |
|---|---|
| `effect("name")` | Create a new `Effect` |
| `@effect(e1, e2, ...)` | Decorate a function to declare its effects |
| `create_handler(*effects)` | Create a synchronous handler |
| `create_async_handler(*effects)` | Create an asynchronous handler |
| `h.on(effect)` | Register a handler function (decorator) |
| `h(caller)` | Run caller with the handler active |
| `effects(fn)` | Get the declared effect set of a function |
| `unhandled_effects(fn, *handlers)` | Get effects not covered by the given handlers |
| `Effect[P, R]` | Effect protocol (parameters `P`, return type `R`) |
| `Handler[V]` | Sync handler protocol |
| `AsyncHandler[V]` | Async handler protocol |
| `Resume[R, V]` | Sync continuation (`k(value) -> V`) |
| `ResumeAsync[R, V]` | Async continuation (`await k(value) -> V`) |

## License

Apache-2.0
