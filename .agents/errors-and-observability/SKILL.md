---
name: errors-and-observability
description: >
  How to handle failures and make systems observable in production. Load
  when adding error handling, logging, metrics, or tracing — or when
  deciding whether to throw, return a result, retry, or fail. Stack-
  agnostic.
---

# Errors and Observability

> Errors are part of the contract. Logs are not for the agent — they
> are for the human reading them at 03:00 during an incident. Treat
> both with that audience in mind.

---

## The Two Kinds of Failure

Distinguish them, because they get handled differently:

- **Expected failures.** Conditions the code must routinely handle:
  validation errors, not-found, payment declined, rate-limited.
  These are part of the function's contract. Encode them in the type
  system, not as exceptions.
- **Unexpected failures.** Conditions the code did not anticipate:
  programmer bugs, broken invariants, corrupted state. These should
  blow up loudly. Do not swallow them.

The default mistake is treating both the same. A "catch everything and
log it" handler hides bugs while routinely-failing paths use exceptions
for control flow. Both ends suffer.

---

## Encoding Expected Failures

The signature tells the caller what can go wrong.

```cpp
// Good — failure is part of the contract.
enum class ChargeError {
    CardDeclined,
    InsufficientFunds,
    InvalidCard,
    GatewayUnavailable  // transient — caller may retry
};

Result<ChargeId, ChargeError> charge_card(const Card& card, Money amount);

// Good — std::optional for simple not-found sentinel
std::optional<User> find_user(const std::string& email);
```

The caller is forced to handle the failure path, or to explicitly
ignore it. No silent swallowing.

---

## Exceptions — When They Fit

Exceptions are correct for *truly unexpected* conditions: out-of-memory,
broken invariants, configuration errors that should crash the process,
panics in unreachable code paths.

If a failure is routine — happens in normal operation, handled at the
caller — an exception is the wrong tool. Use a result type or a
sentinel.

When you do throw:

- Throw a specific, named type. Not the base `Exception` or `Error`.
- Include the context that makes the failure diagnosable: what was
  being attempted, with what inputs.
- Do not include secrets, credentials, or full user data in the message.

When you do catch:

- Catch the specific type you can handle. Never bare `except:` /
  `catch (_)` / `catch (...)`. Bare catches hide bugs.
- The catch site either recovers meaningfully or re-throws with added
  context. "Catch, log, swallow" is almost always wrong.

---

## Error Translation at Boundaries

Errors do not cross package or layer boundaries unchanged. They are
translated.

- An adapter catches the vendor's exception hierarchy and returns the
  domain's `Result<T, DomainError>`.
- A domain function returning a domain error gets translated at the
  HTTP boundary into a status code and a safe response.
- A safe response never includes internal details, stack traces, or
  hints about the architecture.

```
Vendor SDK error  →  Adapter translates to DomainError
DomainError       →  HTTP layer maps to status + safe body
HTTP body         →  Client sees only what is safe to expose
```

Each layer owns its own error type. This is why packages are isolated
in the first place.

---

## Logging — for the Reader

Logs exist to answer "what happened?" during an incident. Optimize for
that reader.

### Levels and what they mean

| Level | Meaning | Example |
|---|---|---|
| `ERROR` | Something failed that needs attention | Database unreachable; payment gateway returned 500 |
| `WARN` | Unusual but handled; may indicate degradation | Retry succeeded after backoff; cache miss when usually a hit |
| `INFO` | High-level state changes worth recording | Service started; user signed in; order placed |
| `DEBUG` | Detail useful when investigating | Function entry/exit; query parameters; intermediate values |
| `TRACE` | Very fine-grained; usually off in production | Per-iteration values in a loop |

The level is not "how loud" — it is *who cares*. If on-call should be
paged, it is `ERROR`. If a developer needs it tomorrow, it is `DEBUG`.

### Structure

Logs are structured, not free-form prose. Each entry has:

- A timestamp (the framework provides this)
- A level
- A short message (the *what*)
- Key-value context (the *details*)
- A correlation ID (trace/request ID) tying related entries together

```json
{
  "ts": "2026-05-18T12:34:56.789Z",
  "level": "ERROR",
  "msg": "payment gateway charge failed",
  "request_id": "req_a1b2c3",
  "user_id": "usr_42",
  "order_id": "ord_99",
  "gateway": "stripe",
  "gateway_code": "card_declined",
  "decline_reason": "insufficient_funds"
}
```

Searching by `order_id` finds every log line about that order. Searching
free-form prose does not.

### What never goes in logs

- Passwords, tokens, API keys, session IDs, signing keys
- Full credit card numbers (PAN), CVV, full bank account numbers
- Personally identifiable information beyond what the project's
  retention and compliance policy permits
- Full request bodies of authenticated requests by default — they
  often contain the above

Redaction is enforced at the logger level, not by remembering to
sanitize at every call site.

---

## Metrics

Logs answer "what happened to this request?" Metrics answer "what is
the system doing?"

The standard set for any non-trivial service:

- **Rate** — requests per second, by endpoint
- **Errors** — error rate, by endpoint and error class
- **Duration** — latency percentiles (p50, p95, p99), not averages
- **Saturation** — queue depth, connection pool usage, memory

Adapter calls get their own metrics: outbound request rate, error rate,
duration. When a vendor degrades, this is how you find out.

---

## Tracing

For systems where one request crosses multiple services or packages:
emit traces. A trace ID at the entry point, propagated through every
call, every log line, every span. Without it, debugging a distributed
failure is archaeology.

---

## Alerting

Alerts wake humans. Earn the right to send one:

- Alert on user-visible impact, not on internal symptoms. "Checkout
  latency above SLO" beats "Redis CPU at 80%".
- Alert on patterns, not single events. One 500 is noise; a sustained
  rate is a problem.
- Every alert has a runbook: what to check, what to do, who else to
  involve.

---

## Failing Closed

When something is broken or ambiguous, deny. This is the security rule,
repeated because it is also the reliability rule:

- An auth check that throws should result in 401/403, not a permissive
  fallthrough.
- A feature flag service that is unreachable should default to the safe
  setting, not the latest experiment.
- A database query that times out should fail the request, not return
  stale or partial data unless that is explicitly the contract.

---

## What an Agent Must Do

- Every error path produces either a typed result or a deliberate
  exception with context. No swallowed errors.
- Every log line is structured, correlated, and free of secrets.
- Every new endpoint or job emits the standard rate/error/duration
  metrics (if the framework provides them — flag if not).
- Failure modes documented in the function header (see
  `third-party-integrations`) match the failures the code actually
  handles.

---

## When to Refuse

Refuse a change, and surface, when:

- An error is caught and silently swallowed.
- A bare `catch` / `except` / `panic-recover` is added.
- Logs would include secrets, PII beyond policy, or full credit card
  numbers.
- An auth or authz check would fail open.
- The change makes a failure mode silent that was previously surfaced.

---

## The Errors-and-Observability Mantra

> **"Encode expected failures. Throw for the unexpected. Translate at boundaries. Log for the human at 03:00. Fail closed."**
