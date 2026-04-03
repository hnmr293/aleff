# aleff1

Algebraic effects for Python — deep, stateful, one-shot handlers via greenlet-based delimited continuations.

## Features

- **Deep handlers** — effects propagate through nested function calls without annotation
- **Stateful handlers** — handler functions can execute code after `resume`, enabling patterns like transactions and reverse-mode AD
- **Sync and async** — both synchronous (`handler`) and asynchronous (`async_handler`) handlers are supported
- **Effect composition** — `@effect(step1, step2)` collects effect sets transitively from decorated functions
- **Introspection** — `effects(fn)` and `unhandled_effects(fn, h)` for querying and validating effect coverage
- **Typed** — effect parameters and return types are checked by type checkers (pyright, ty)
- **No macros, no code generation** — pure Python library built on [greenlet](https://github.com/python-greenlet/greenlet)

## Requirements

- Python >= 3.12
- greenlet >= 3.3.2

## Installation

from source:

```sh
git clone https://github.com/hnmr293/aleff1.git
cd aleff1
pip install .
```

## Quick start

```python
from aleff1 import effect, Effect, Resume, create_handler

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

## How it works

Effects are declared as typed values and invoked like regular function calls. A `handler` intercepts these calls via greenlet-based context switching:

1. Business logic runs in a greenlet
2. When an effect is invoked, control switches to the handler
3. The handler processes the effect and calls `resume(value)` to return a value
4. If the handler returns without calling `resume`, the computation is **aborted** (early exit)

Because handlers use greenlets (not exceptions), the control flow is:
- **Transparent** — no `yield`, `await`, or special syntax in business logic
- **Stateful** — code after `resume` runs after the rest of the computation completes, enabling reverse-order execution (useful for backpropagation, transactions, etc.)

## Examples

See [`examples/`](examples/) for demonstrations:

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
| `Resume[R, V]` | Sync continuation (`k(value) -> V`) |
| `ResumeAsync[R, V]` | Async continuation (`await k(value) -> V`) |

## License

Apache-2.0
