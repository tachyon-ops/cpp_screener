---
name: architecture
description: >
  Architectural fundamentals for any codebase: package and module isolation,
  dependency direction, ports and adapters, public versus internal surfaces.
  Load when designing a new package, adding cross-package dependencies,
  changing public exports, or evaluating whether code lives in the right
  place. Stack-agnostic — the principles are identical in TypeScript, Rust,
  C++, Python, Go.
---

# Architecture

> One codebase, many packages. Each package owns a domain. Boundaries are
> explicit, enforced, and respected. Dependencies flow one way.

---

## The Principle

A codebase is a graph of packages (or modules, crates, libraries — the name
depends on the stack). Each node owns a coherent responsibility. Each edge
is a deliberate dependency from a higher-level package to a lower-level one.

The principle is identical across stacks:

| Stack | Unit of isolation | Public surface | Internal surface |
|---|---|---|---|
| TypeScript (pnpm/turbo) | Workspace package | `package.json` `exports` field | Files outside `exports` |
| Rust | Crate in a workspace | `pub` items in `lib.rs` / module root | `pub(crate)` and below |
| C++ | Internal library / target | Public headers in `include/<lib>/` | `src/` private headers |
| Python | Namespace package | `__init__.py` exports / `__all__` | `_private` modules and names |
| Go | Module | Exported identifiers (capitalized) | Unexported (lowercase) |

The unit, syntax, and tooling change. The discipline does not.

---

## Non-Negotiable Architectural Rules

1. **No deep imports across packages.** A consumer imports from a package's
   public surface only. Reaching into another package's internals is a
   design failure. Reject it.
2. **No circular dependencies.** Between packages, between modules, between
   classes. Cycles are a structural defect. If you find one, do not work
   around it — pause and surface it.
3. **Dependencies flow one direction.** Higher-level packages depend on
   lower-level ones. Never the reverse. Domain depends on infrastructure
   abstractions, not implementations.
4. **A package has one reason to change.** Cohesion is the test. If two
   responsibilities live in one package and change for different reasons,
   they belong in two packages.
5. **Internal is internal.** If it is not exported through the declared
   public surface, no one outside the package may use it. Period.

---

## Layering

Most non-trivial codebases divide into three layers. The names vary; the
shape is the same.

```
┌────────────────────────────────────────────────┐
│  Application / Composition / Entry             │  ← composes layers below
│  (apps, services, CLIs, web routes)            │
├────────────────────────────────────────────────┤
│  Domain / Core / Logic                         │  ← pure logic, no I/O
│  (entities, use cases, algorithms)             │
├────────────────────────────────────────────────┤
│  Infrastructure / Adapters / Platform          │  ← I/O lives here
│  (database, HTTP clients, filesystem, queues)  │
└────────────────────────────────────────────────┘
```

**Dependency direction:** Application depends on Domain. Domain depends on
abstract Ports. Infrastructure implements those Ports. Domain never imports
Infrastructure.

This is the Ports and Adapters pattern (sometimes called Hexagonal or Clean
Architecture). Names vary, principle is fixed.

---

## Ports and Adapters

A **Port** is an interface owned by the Domain that names a capability it
needs (`UserRepository`, `Clock`, `PaymentGateway`).

An **Adapter** is an Infrastructure implementation of a Port
(`PostgresUserRepository`, `SystemClock`, `StripePaymentGateway`).

The Domain depends on the Port. The Application wires the Adapter in at
composition time. The Domain never knows which Adapter it received.

This is what makes the code portable, testable, and swappable. It is also
how you keep the functional core pure (see `functional-core` skill).

---

## Public vs Internal Surface — Expression by Stack

### TypeScript (pnpm / turbo workspace)

```jsonc
// packages/orders/package.json
{
  "name": "@app/orders",
  "exports": {
    ".": "./src/index.ts",
    "./types": "./src/types.ts"
  }
}
```

Anything not listed in `exports` is internal. Deep imports like
`@app/orders/src/internal/state` are forbidden and should be caught by
lint rules.

### Rust (workspace with crates)

```rust
// crates/orders/src/lib.rs
pub mod api;          // ← public surface
pub mod types;        // ← public surface
pub(crate) mod state; // ← internal to this crate
mod parser;           // ← private to this module
```

Only `pub` items at the crate root or in `pub mod` paths are accessible
from other crates.

### C++ (CMake target with public headers)

```
libs/orders/
├── include/orders/      ← public headers (target_include_directories PUBLIC)
│   ├── api.hpp
│   └── types.hpp
└── src/                 ← private (target_include_directories PRIVATE)
    ├── state.hpp
    └── parser.cpp
```

Only `include/orders/*.hpp` is consumable. Consumers `#include <orders/api.hpp>`,
never `#include "../../libs/orders/src/state.hpp"`.

### Python (namespace package)

```python
# orders/__init__.py
from ._api import place_order, cancel_order
from ._types import Order, OrderStatus

__all__ = ["place_order", "cancel_order", "Order", "OrderStatus"]

# orders/_state.py    ← internal (underscore prefix)
# orders/_parser.py   ← internal
```

The underscore convention is enforced by code review and lint
configuration (`flake8`/`ruff` rules against importing `_private`).

---

## When Adding a New Package

Before creating one, ask:

1. **Does it have a single, nameable responsibility?** If you cannot name
   it in three words, it is doing too much.
2. **Will more than one place consume it?** A package with one consumer is
   often premature; the code probably belongs inside that consumer.
3. **What layer does it live in?** Domain, Application, or Infrastructure.
4. **What does it depend on?** List the dependencies before writing code.
   If the list crosses layers in the wrong direction, redesign.
5. **What is its public surface?** Write the public API first as types and
   signatures. The implementation comes after.

---

## When Adding a Cross-Package Dependency

```
1. Check the layer rule: is this dependency flowing the correct direction?
2. Check for cycles: does the target depend on this package, directly or
   transitively? If yes, redesign — extract the shared concept to a lower
   layer that both can depend on.
3. Use only the public surface of the target package.
4. If the dependency feels wrong, it probably is. Pause and surface it.
```

---

## Anti-Patterns to Refuse

- **God package.** A `common`, `utils`, or `shared` package that everything
  depends on and that grows without bound. Break it up by domain.
- **Backwards dependency.** Domain importing Infrastructure. Stop and
  introduce a Port.
- **Cross-package internals.** Importing `@app/orders/src/internal/x`,
  `crate::orders::state` from outside the crate, `#include
  "../orders/src/state.hpp"`. Reject the change.
- **Hidden coupling.** Two packages that share a database table, a global
  singleton, or a mutable module-level state without a declared interface.
  This is a dependency in disguise — make it explicit or remove it.
- **Premature package.** A new package created for a single function. Put
  it in the consumer until duplication forces extraction.

---

## The Architecture Mantra

> **"One package, one reason. Dependencies flow down. Public is a promise. Internal is a secret."**
