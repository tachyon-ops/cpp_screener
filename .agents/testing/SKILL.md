---
name: testing
description: >
  What to test, at what layer, why, and how. Load when writing new tests,
  changing test strategy, deciding whether something needs a test, or
  evaluating test failures. Stack-agnostic.
---

# Testing

> Tests exist to give you confidence that the code does what it claims,
> and to alert you when that stops being true. Tests that do not serve
> those two purposes are noise. Delete them.

---

## The Goals

A test suite has three jobs:

1. **Prove the contract.** When a function or system claims to do
   something, a test demonstrates that it does.
2. **Catch regressions.** When a future change breaks an established
   behaviour, a test fails before the code ships.
3. **Document the intent.** A well-written test is the clearest
   specification of how a function is meant to be used.

Tests that do not serve at least one of these are not assets. They are
liabilities — they slow CI, they break for the wrong reasons, they get
deleted in panic.

---

## The Pyramid (Approximately)

The classic pyramid is a useful default, not a law:

```
       ╱╲          End-to-end / system   (few, slow, full-stack)
      ╱──╲
     ╱────╲        Integration           (some, medium, across boundaries)
    ╱──────╲
   ╱────────╲      Unit / pure-core      (many, fast, in-process)
  ╱──────────╲
```

- **Unit tests** dominate. They are cheap to write, fast to run, and
  pinpoint failures. Pure-core code (see the `functional-core` skill)
  is ideal for this layer.
- **Integration tests** cover seams: a real database connection, a
  real HTTP call to a vendor's sandbox, a real filesystem operation.
  Fewer because they are slower, but they catch what unit tests cannot.
- **End-to-end tests** cover full user journeys against a running
  system. Few, expensive, valuable for critical paths only.

The exact ratio depends on the project. The principle: catch failures
at the lowest layer that can catch them. A bug provable in a unit test
should not require an E2E test.

---

## What to Test

Test the contract, not the implementation. The test should survive a
refactor that preserves behaviour.

| Worth testing | Not worth testing |
|---|---|
| Edge cases of a pure function | That a getter returns the field |
| Error paths and failure modes | That a constructor stores its parameters |
| Boundary conditions (empty, max, off-by-one) | The framework itself |
| Behaviour at integration seams | Mocked behaviour of code you own |
| Regression cases for fixed bugs | Implementation details the public API does not expose |
| Authorization on protected operations | UI pixel positions (use snapshot tests sparingly) |

The "implementation details" trap is the most common: a test that
breaks every time you rename a private variable is testing the wrong
thing. Test through the public surface.

---

## What Makes a Good Test

- **Fast.** Unit tests run in milliseconds. Slow tests stop being run.
- **Deterministic.** Same inputs, same result, every time. No flakiness,
  no time-of-day dependence, no network unless intended.
- **Isolated.** One test cannot influence another. No shared mutable
  state between tests. Setup and teardown are explicit.
- **Readable.** A failing test should tell you what was expected and
  what happened in one glance. The test name describes the behaviour
  being asserted.
- **Narrow.** One assertion's worth of intent per test. A test that
  fails on twelve different conditions is twelve tests crammed into one.

### The Arrange-Act-Assert shape

```
# Arrange — set up inputs and dependencies
order = make_order(lines=[line(price=10), line(price=20)])
discount = Discount(percent=Decimal("0.1"))

# Act — call the thing under test
total = compute_order_total(order.lines, discount.percent)

# Assert — check the contract
assert total == Money("27.00")
```

Three sections, in order. No conditionals, no loops, no try/except in
the test body unless that itself is the contract being verified.

---

## Test Doubles — Used Sparingly

A "test double" is a stand-in for a real dependency: stub, fake, mock,
spy. They have legitimate uses and serious failure modes.

- **Fakes** (in-memory implementations of an interface) are the safest.
  They behave like the real thing without the I/O cost.
- **Stubs** (return canned answers) are fine for narrow, well-defined
  cases.
- **Mocks with expectation matching** are the most fragile. They couple
  the test to the call sequence, not the outcome. Use only when the
  call itself is the contract (e.g., "this code must publish a specific
  event").

The smell: if a test mocks the very code it claims to verify, it tests
nothing. Push the mocks to the edges. Test the pure core directly with
real values.

This is another reason the functional-core pattern matters: pure code
needs no doubles.

---

## Integration Tests

These run against real boundaries. The trade-off is value vs. cost.

- **Database tests** run against a real database instance — same engine,
  same migrations as production. SQLite-as-substitute-for-Postgres lies
  in subtle ways.
- **Vendor tests** run against sandbox APIs where available, recorded
  fixtures otherwise. Record once, replay always. Re-record only when
  the contract changes.
- **HTTP boundary tests** spin up the real router, the real middleware,
  the real authentication, with a fake adapter at the very edge.

Integration tests are where security baselines are verified end-to-end:
that auth is enforced, that authz cannot be bypassed, that injection
inputs are parameterized.

---

## End-to-End Tests

E2E tests run the full system. They are slow, expensive to maintain,
and catch things nothing else can.

Reserve them for:

- Critical user journeys (sign up, pay, withdraw)
- Cross-service flows where a contract spans multiple processes
- Regression coverage for production incidents

A handful of well-chosen E2E tests beats a hundred flaky ones.

---

## When a Test Fails

```
1. Read the failure. Understand what it says.
2. Reproduce it locally. A test that only fails in CI is not a passing
   test — it is a heisenbug.
3. Decide: is the test wrong, or is the code wrong?
4. If the code is wrong — fix the code. Do not change the test to match.
5. If the test is wrong — fix the test deliberately. Note why in the
   commit. "Updated test" is not a sufficient message.
6. Never disable a test. Either fix it or delete it with reason.
```

---

## Test Discipline for AI Agents

- Write the test alongside the change, in the same commit. Tests
  shipped later get shipped never.
- Run the relevant tests before claiming the task complete.
- If you cannot run tests, say so. "Untested — please run X to verify"
  is honest.
- A change that breaks existing tests is incomplete unless the test
  changes are deliberate and explained.

---

## When to Refuse

Refuse a change, and surface, when:

- A test is being disabled to make a change pass.
- The change reduces existing test coverage of a public surface.
- An "untested for now" change involves auth, money, data integrity, or
  security boundaries.

---

## The Testing Mantra

> **"Test the contract. Fast, isolated, deterministic. The test names the behaviour. Failure tells you why."**
