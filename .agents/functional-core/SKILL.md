---
name: functional-core
description: >
  How to structure code so business logic stays pure and side effects live
  at the edges. Load when writing or restructuring any non-trivial
  function, when adding I/O, or when a function "needs the database" to
  do its job. Stack-agnostic — applies in TypeScript, Rust, C++, Python,
  Go.
---

# Functional Core, Imperative Shell

> Logic at the center. Effects at the edges. Easy to test, easy to read,
> easy to move.

---

## The Principle

A program has two kinds of code:

- **Pure logic.** Given the same inputs, returns the same outputs.
  Touches nothing outside its parameters. No clock, no network, no
  filesystem, no random, no global state.
- **Effects.** Everything else — reading from a database, calling an API,
  writing a file, getting the current time, generating a UUID, logging.

The principle: push effects to the edge, keep the center pure. The center
becomes trivially testable, portable, and reusable. The shell becomes
small enough that integration tests cover what unit tests cannot.

This is not a stylistic preference. Pure functions have properties that
effectful ones do not: they compose without surprises, they can be cached,
parallelized, retried, replayed, and reasoned about locally.

---

## What Counts as a Side Effect

If a function does any of these, it has effects:

- Reads or writes the database, filesystem, network, environment variables
- Reads the current time, generates randomness or UUIDs
- Mutates a shared variable, module-level state, or any parameter
- Logs, prints, or emits metrics
- Throws an exception based on environmental state (vs. invalid input)

If a function takes data in and returns data out, with none of the above,
it is pure. Build as much of your logic from pure functions as possible.

---

## The Shape

```
┌─────────────────────────────────────────────────┐
│  Shell (effectful)                              │
│                                                 │
│  1. Read inputs from the world (DB, HTTP, FS)   │
│  2. Pass them to the pure core                  │
│  3. Take the result                             │
│  4. Write changes back to the world             │
│                                                 │
│   ┌───────────────────────────────────────┐     │
│   │  Core (pure)                          │     │
│   │                                       │     │
│   │  Decisions, calculations, validation, │     │
│   │  transformations, business rules      │     │
│   │                                       │     │
│   └───────────────────────────────────────┘     │
│                                                 │
└─────────────────────────────────────────────────┘
```

The core never asks "what's in the database?" — it is given the data.
The shell never decides — it executes the decisions the core returned.

---

## Example: Pricing an Order

**Bad.** Pure logic and effects mixed together.

```python
def price_order(order_id: str) -> Money:
    order = db.fetch_order(order_id)              # effect
    discount = db.fetch_discount(order.user_id)   # effect
    subtotal = sum(line.price for line in order.lines)
    total = subtotal * (1 - discount.percent)
    db.write_audit_log(order_id, total)           # effect
    return total
```

To test this, you need a database, fixtures, and mocks. Every change to
the rules requires running through the I/O layer.

**Good.** Pure decision, effectful shell.

```python
# Pure core — no I/O, no globals, no exceptions for environmental state.
def compute_order_total(lines: Sequence[Line], discount_percent: Decimal) -> Money:
    subtotal = sum((line.price for line in lines), Money.zero())
    return subtotal * (Decimal(1) - discount_percent)

# Shell — composes the pure core with effects.
def price_order(order_id: OrderId, db: OrderRepository, audit: AuditLog) -> Money:
    order = db.fetch_order(order_id)
    discount = db.fetch_discount(order.user_id)
    total = compute_order_total(order.lines, discount.percent)
    audit.record(order_id, total)
    return total
```

The pure function `compute_order_total` is testable with one line. The
shell stays trivial — fetch, compute, write.

---

## How to Refactor Toward Purity

When you encounter a function that mixes logic and effects:

```
1. Identify the inputs the logic actually needs (the values, not the sources)
2. Extract those inputs as parameters of a new pure function
3. Move the I/O calls up one level into the caller (or into a new shell function)
4. Pass the pure function's result back to the shell to act on
```

If the logic depends on the result of an I/O call mid-computation, the
shell does both calls and passes both results to the core. The core
remains a function of its inputs.

---

## Effects That Cannot Be Avoided

Some effects are required for the program to do anything useful. Three
techniques to keep them under control:

### Ports and Adapters

The core declares an interface (the Port) for what it needs. The shell
provides the implementation (the Adapter). The core is parameterized
over the interface and never sees the concrete type.

```rust
// Core declares the capability it needs.
pub trait Clock {
    fn now(&self) -> Timestamp;
}

// Core uses it as a parameter — never as a global.
pub fn is_expired(token: &Token, clock: &impl Clock) -> bool {
    token.expires_at < clock.now()
}

// Shell provides the implementation.
pub struct SystemClock;
impl Clock for SystemClock {
    fn now(&self) -> Timestamp { Timestamp::now_utc() }
}
```

In tests, swap in a `FakeClock` that returns a fixed value. No mocking
framework needed.

### Inject the Result, Not the Source

If a function needs "the current user," do not have it call
`getCurrentUser()` (an effect). Have the caller pass the user. The
function's signature now declares the dependency honestly.

### Return Intent, Let the Shell Execute

The core returns a description of what should happen. The shell
performs the actions.

```typescript
// Core returns intent.
type Decision =
  | { kind: 'approve'; amount: Money }
  | { kind: 'reject'; reason: string }
  | { kind: 'request_review'; assignTo: UserId };

function decide(application: Application, rules: RuleSet): Decision { ... }

// Shell executes.
const decision = decide(app, rules);
switch (decision.kind) {
  case 'approve':       await db.approve(app.id, decision.amount); break;
  case 'reject':        await notify.reject(app.id, decision.reason); break;
  case 'request_review': await queue.assign(app.id, decision.assignTo); break;
}
```

The core is pure. The shell does I/O. The decision logic is testable
without touching a database.

---

## Mutation

Mutation is a side effect even if it stays local. Default to immutable
values: return new data instead of modifying existing data. Mutation is
fine where the language and performance demand it (large buffers, hot
loops), but it should be localized and documented.

Never mutate a parameter. The signature does not say so, and the caller
will not expect it.

---

## When Purity Is Genuinely Wrong

Purity is a tool, not a religion. Some places are inherently effectful:

- Composition roots (`main`, application entry)
- Adapters that wrap a vendor or platform
- UI event handlers at the very edge

Do not contort these into pure shapes. The point is that they are *thin*
and *named*. The thick, complex code is pure; the effectful code is small
and obvious.

---

## The Functional-Core Mantra

> **"Decide in the core. Act in the shell. Inject what you need. Return what should happen."**
