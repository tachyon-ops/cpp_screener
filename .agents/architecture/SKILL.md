---
name: architecture
description: >
  Architectural fundamentals for the cpp_screener codebase: package and module isolation,
  dependency direction, ports and adapters, public versus internal surfaces.
  Load when designing a new C++ module, adding cross-module dependencies,
  changing public exports, or evaluating whether code lives in the right place.
---

# Architecture

> One codebase, many modules. Each module owns a domain. Boundaries are
> explicit, enforced, and respected. Dependencies flow one way.

---

## The Principle

A codebase is a graph of modules. Each node owns a coherent responsibility. Each edge
is a deliberate dependency from a higher-level package to a lower-level one.

| Stack | Unit of isolation | Public surface | Internal surface |
|---|---|---|---|
| C++ | Internal library / target | Public headers in `include/trader/<lib>/` | `src/<lib>/` private headers |

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

```text
┌────────────────────────────────────────────────┐
│  Application / Composition / Entry             │  ← composes layers below
│  (trader-engine main, web routes, alerts)      │
├────────────────────────────────────────────────┤
│  Domain / Core / Logic                         │  ← pure logic, no I/O
│  (screens, regime, core types)                 │
├────────────────────────────────────────────────┤
│  Infrastructure / Adapters / Platform          │  ← I/O lives here
│  (SQLite store, Saxo API, WebSocket streaming) │
└────────────────────────────────────────────────┘
```

**Dependency direction:** Application depends on Domain. Domain depends on
abstract Ports. Infrastructure implements those Ports. Domain never imports
Infrastructure implementations.

This is the Ports and Adapters pattern (sometimes called Hexagonal or Clean
Architecture). Names vary, principle is fixed.

---

## Ports and Adapters

A **Port** is an interface owned by the Domain that names a capability it
needs (e.g., `BrokerAdapter`, `TimeSeriesStore`).

An **Adapter** is an Infrastructure implementation of a Port
(e.g., `SaxoBrokerAdapter`, `SQLiteStore`).

The Domain depends on the Port. The Application wires the Adapter in at
composition time. The Domain never knows which Adapter it received.

---

## Public vs Internal Surface — Expression in C++

### C++ (CMake target with public headers)

```text
trader/
├── include/trader/broker/      ← public headers (target_include_directories PUBLIC)
│   ├── broker_adapter.hpp
│   └── saxo_adapter.hpp
└── src/broker/                 ← private (target_include_directories PRIVATE)
    └── saxo_adapter.cpp
```

Only `include/trader/**/*.hpp` is consumable. Consumers `#include <trader/broker/broker_adapter.hpp>`,
never `#include "../../src/broker/internal_state.hpp"`.

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

```text
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
- **Cross-package internals.** `#include "../src/state.hpp"`. Reject the change.
- **Hidden coupling.** Two packages that share a database table, a global
  singleton, or a mutable module-level state without a declared interface.
  This is a dependency in disguise — make it explicit or remove it.
- **Premature package.** A new package created for a single function. Put
  it in the consumer until duplication forces extraction.

---

## The Architecture Mantra

> **"One module, one reason. Dependencies flow down. Public is a promise. Internal is a secret."**
