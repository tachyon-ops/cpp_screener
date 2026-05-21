---
name: third-party-integrations
description: >
  How to integrate any third-party API, SDK, or service so the rest of
  the codebase depends on your abstraction and not on the vendor. Load
  when calling any external HTTP, gRPC, SDK, database driver, message
  bus, or platform API. Defines the mandatory in-body documentation
  convention for third-party calls. Stack-agnostic.
---

# Third-Party Integrations

> The codebase owns its abstractions. Vendors own their APIs. The two
> meet at a thin, documented seam — never anywhere else.

---

## The Principle

Every third-party API is a moving target. Endpoints change. Auth flows
change. Rate limits change. SDKs deprecate. Pricing tiers gate features.
The vendor's failure modes are not yours, and your business logic must
not be coupled to them.

So: wrap every external dependency behind a small, owned interface.
The rest of the codebase talks to the interface. Only one place in the
codebase talks to the vendor.

This is the same Ports-and-Adapters principle from the architecture and
functional-core skills, applied specifically to external boundaries.

---

## The Wrapping Discipline

For every third-party API:

```
1. Define a Port — an interface or trait owned by your codebase that
   names the capability in your own terms.
2. Write one Adapter — the only place in the codebase that imports the
   vendor's SDK or constructs HTTP calls to its endpoints.
3. Translate types at the boundary. The vendor's `User` is not your
   `User`. Map at the seam.
4. Translate errors at the boundary. The vendor's exception hierarchy
   does not leak into your domain.
5. Have the rest of the code depend only on the Port.
```

Example:

```cpp
// Port — owned by your code (domain interface).
class BrokerAdapter {
public:
    virtual ~BrokerAdapter() = default;
    virtual Result<InstrumentInfo, BrokerError> lookup_symbol(const std::string& sym) = 0;
    virtual Result<void, BrokerError> refund(ChargeId charge, std::optional<Money> amount) = 0;
};

// Adapter — only component that knows about Saxo OpenAPI.
class SaxoBrokerAdapter : public BrokerAdapter {
public:
    Result<InstrumentInfo, BrokerError> lookup_symbol(const std::string& sym) override {
        // Third-party APIs:
        //   - Saxo OpenAPI: GET /ref/v1/instruments?Keywords={sym}
        //     docs: https://www.developer.saxo/openapi/referencedocs/ref/v1/instruments
        //     auth: OAuth2 bearer token from TokenStore
        //     rate limits: 60 req/m
        //     failure modes handled: 401 (needs refresh), 429 (rate limit), 5xx
        try {
            auto response = http_client_.get("/ref/v1/instruments?Keywords=" + sym);
            if (response.status == 200) {
                return Ok(parse_instrument(response.body));
            }
            return Err(translate_saxo_error(response));
        } catch (const std::exception& e) {
            return Err(BrokerError::ConnectionFailed);
        }
    }
    // ...
};
```

The domain code calls `gateway.charge(...)` and knows nothing about
Stripe. Swapping to a different provider is a new adapter, not a rewrite.

---

## The Header Documentation Convention

Every function that calls a third-party API documents the call at the top
of the function body. This is mandatory.

The block lists, for each external dependency the function touches:

- **Service or library name**
- **Endpoint / method / function called**
- **Link to the official docs**
- **Auth mechanism** (env var, header, etc.)
- **Known failure modes handled** (and any deliberately not handled)
- **Rate limits or quotas relevant to this call**

```cpp
Result<Balance, FetchError> fetch_account_balance(AccountId account_id, SaxoClient& client) {
    // Third-party APIs:
    //   - Saxo OpenAPI: GET /port/v1/balances?ClientKey={key}
    //     docs: https://www.developer.saxo/openapi/referencedocs/port/v1/balances
    //     auth: OAuth2 bearer token from SaxoTokenStore
    //     rate limit: 240 requests / minute per app
    //     failure modes handled: 401 (token expired -> refresh),
    //                            429 (rate limited -> backoff),
    //                            5xx (transient -> retry with jitter)
    ...
}
```

Why this is non-negotiable:

- A reader knows immediately what the function depends on.
- Auditing external dependencies is one `grep` away.
- Migrations and outages can be scoped without spelunking.
- The author is forced to read the docs before writing the call.

---

## Failure Modes

Handle the failure modes the vendor actually documents. Not the ones
you imagine.

```
1. Read the vendor's error reference.
2. Enumerate which errors are:
   - Retryable (transient, idempotent) — retry with backoff
   - Recoverable (caller can react) — translate to your error type
   - Fatal (config or auth problem) — surface clearly, do not retry
3. Make the policy explicit in code, not in comments only.
```

Common categories worth recognizing across vendors:

| Category | Examples | Policy |
|---|---|---|
| Auth | 401, 403, invalid token | Refresh once, then fail with clear error |
| Rate limit | 429, quota exceeded | Backoff with jitter, respect `Retry-After` |
| Validation | 4xx with field errors | Translate to domain error, do not retry |
| Server error | 500, 502, 503, 504 | Retry with backoff, only if idempotent |
| Network | Connection refused, timeout | Retry if idempotent; surface if not |
| Not found | 404 | Often a domain `None`, not an error |

---

## Retries, Timeouts, Backoff

Every outbound call has:

- **An explicit timeout.** Never an unbounded `await`. The default
  timeout of most HTTP clients is "forever" — set one.
- **A retry policy** matched to idempotency. Reading is safely
  retryable. Writing is not, unless the endpoint supports idempotency
  keys.
- **Jittered exponential backoff** between retries. Without jitter,
  retries from many callers synchronize into a thundering herd.

If the vendor does not document its idempotency guarantees, assume none.
Surface the question to the human; do not silently retry writes.

---

## Secrets

Credentials for third-party services live in environment variables or a
secret manager. Never in code, never in config files, never in
fixtures, never in commit history.

The adapter is the only place that reads them. The rest of the code
receives a constructed adapter, not credentials.

See the `security-fundamentals` skill for full secret-handling rules.

---

## Versions and Drift

- Pin SDK and API versions explicitly. Floating versions are deferred
  breakage.
- When the vendor publishes a breaking change, treat it as a project
  task — read the migration guide, plan the change, do it deliberately.
- Subscribe to the vendor's deprecation feed where possible. Surface
  upcoming deprecations as `NOTE:` in the adapter.

---

## When to Pause and Ask

Pause and surface the question when:

- The vendor's documentation contradicts itself.
- A required failure mode is undocumented.
- The integration would couple multiple domains to one vendor.
- The vendor charges per call and the call site is in a hot path.
- The integration requires storing data subject to compliance constraints
  (PII, payment data, health data) — security review is needed.

---

## The Integrations Mantra

> **"One adapter per vendor. Document at the seam. Handle the failures the docs name. Never leak the vendor into the domain."**
