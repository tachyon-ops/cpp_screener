---
name: api-design
description: >
  How to design and evolve public surfaces — the exported functions, types,
  classes, modules, and endpoints that other code depends on. Load when
  adding, changing, or deprecating any exported symbol, public HTTP/gRPC
  endpoint, or library entry point. Stack-agnostic.
---

# API Design

> Every exported symbol is a contract. Contracts are kept or broken — never
> bent. Breaking a contract requires deliberate, versioned, communicated
> action.

---

## What Counts as an API

Anything used from outside its owning package or module is an API:

- Exported functions, types, classes, constants
- HTTP, gRPC, message-bus endpoints
- CLI flags, environment variables, config schemas
- Database table and column names consumed by other services
- File formats produced or consumed across processes
- Public events on a message bus or event stream

If something else depends on its shape, it is an API. Treat it as one.

---

## The Three Rules

1. **Public is forever (within a version).** Once exported, the signature,
   types, and observable behaviour are a promise. Bend the implementation,
   never the contract.
2. **Additions are safe. Removals and changes are not.** Adding a new
   function, an optional parameter, or a new variant to an open union is
   additive. Removing, renaming, narrowing a type, or changing semantics
   is breaking.
3. **Internal changes freely.** If a function is not part of the public
   surface, refactor it without ceremony. That is why isolation matters.

---

## Designing a New API

Write the signature before the implementation. Read it aloud. Ask:

- **Is the name accurate?** A reader who sees only the signature should
  predict the behaviour.
- **Are the inputs minimal?** Every parameter is justified. No "options
  bag" with sixteen optional flags that means sixteen different functions.
- **Is the return type honest?** If it can fail, the return type says so
  (`Result`, `Either`, `Option`, exception contract documented).
- **Is it composable?** Can the result feed another function naturally?
  Or does the consumer have to unpack and repack?
- **What happens on bad input?** Document or encode every failure mode.
- **What are the side effects?** Named, isolated, or absent. See the
  `functional-core` skill.

---

## Signature Quality Checks

| Smell | Fix |
|---|---|
| `function doStuff(opts: any)` | Replace `any` with a closed type. Name `opts` for what it carries. |
| Five boolean parameters | Replace with a single discriminated union of intents. |
| `null` and `undefined` both meaningful | Pick one. Document. |
| Returns `T | undefined` with no docs on when | Encode the failure: `Result<T, E>` or `Option<T>` with the reason. |
| Function takes a `User` and reads only `user.id` | Take `userId` directly. Smaller contract. |
| Generic name (`process`, `handle`, `manage`) | Rename to the actual action. |
| Output mutates an input | Return a new value. Mutation in signatures is a contract too — usually the wrong one. |

---

## Versioning and Stability

Three stability tiers, declared explicitly in docs or via tooling:

- **Stable.** Public, supported, follows semver (or your project's
  equivalent). Breaking changes require a major version bump.
- **Unstable / Experimental.** Public but marked as subject to change.
  Consumers opt in knowing the risk.
- **Internal.** Not part of the public surface. Free to change.

Mark unstable APIs explicitly:

```cpp
/**
 * @brief [Experimental] May change without notice.
 */
void new_thing(...);
```

---

## Evolving an API Without Breaking It

The default move is **additive**:

- Add a new function, leave the old one in place
- Add a new optional parameter at the end
- Add a new variant to an open enum or union (callers must handle unknown
  variants — design for this)
- Add a new field to a response object (callers must ignore unknown fields)
- Add a new endpoint version (`/v2/orders`) alongside the old one

When breakage is unavoidable:

```
1. Mark the old API as deprecated, with the replacement named in the docs
2. Document the migration path
3. Wait at least one minor version (or one release cycle)
4. Remove only in a major version bump, announced in the changelog
```

Never silently change semantics of an existing signature. It is worse than
breaking the build — it breaks production at runtime.

---

## HTTP / gRPC / Message-Bus APIs

The same rules apply with sharper teeth, because clients are out of your
build system:

- **Versioned paths or headers** from day one: `/v1/...`, `Accept:
  application/vnd.app.v1+json`, gRPC service names like `OrdersV1`.
- **Backwards-compatible response evolution.** Add fields; never rename
  or remove without a new version.
- **Open enums.** Document that unknown values must be tolerated.
- **Idempotency** for any non-GET endpoint that could be retried — accept
  an `Idempotency-Key` header and dedupe on it.
- **Error responses are part of the contract.** Document the shape, the
  codes, and what each means. Do not leak stack traces.

---

## Documentation

Every public symbol has a docstring that covers:

1. **What it does** — in one line.
2. **When to use it** — and when to use something else.
3. **Inputs** — invariants and constraints not captured by types.
4. **Outputs** — including all failure modes.
5. **Side effects** — if any.

```cpp
/**
 * @brief Transfer @p amount between two accounts, atomically.
 * 
 * Use this when both accounts are in the same ledger. For cross-ledger
 * transfers use cross_ledger::initiate_transfer.
 * 
 * @param from_id Source account ID.
 * @param to_id Destination account ID.
 * @param amount Amount to transfer (must be positive).
 * @return Result containing TransferId on success, or TransferError indicating
 *         insufficient funds, account frozen, or ledger mismatch.
 * 
 * @note Side effects: writes one row to transfers, updates two rows in
 *       accounts, emits a TransferCompleted event.
 */
Result<TransferId, TransferError> transfer_funds(AccountId from_id, AccountId to_id, Money amount);
```

---

## When to Refuse

Refuse the change, and surface the conflict, when:

- An edit silently changes the observable behaviour of a public function.
- A type narrowing removes a previously accepted input.
- An HTTP response field is renamed or removed without a new version.
- A required parameter is added to an existing function.
- A package's public surface grows by something that should be internal.

---

## The API Mantra

> **"Add freely. Change deliberately. Remove rarely. Document always."**
