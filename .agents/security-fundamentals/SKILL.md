---
name: security-fundamentals
description: >
  The non-negotiable security baseline for any codebase that handles user
  data, authentication, money, or external input. Load when adding or
  modifying anything involving auth, sessions, secrets, input handling,
  database queries, file uploads, or external requests. Stack-agnostic.
  These are not opinions — they are the distilled rules from OWASP,
  industry post-mortems, and decades of breaches.
---

# Security Fundamentals

> Security is not a feature. It is a property of the whole system. Every
> function that handles user input, every endpoint that authorizes a
> request, every secret that touches disk, every query that meets data
> from the outside — all are part of the boundary. Treat them that way.

---

## The Baseline

These rules are the minimum. They do not replace a threat model, a
security review, or a penetration test for systems that warrant them.
They are what every project does by default.

1. **Authenticate every request that needs it.** No "internal" endpoint
   is exempt — internal networks get breached.
2. **Authorize every action.** Authentication says who you are.
   Authorization says what you may do. They are different. Both are
   required.
3. **Validate every input.** Untrusted data is everything from outside
   the function's caller — HTTP bodies, query strings, headers, file
   contents, environment, message-bus payloads, even other services.
4. **Parameterize every query.** Never concatenate user input into SQL,
   shell commands, file paths, regex, or any interpreter input.
5. **Encrypt secrets at rest and in transit.** TLS for transport.
   Encrypted, access-controlled storage for keys, tokens, credentials.
6. **Log security-relevant events.** Auth attempts, authz failures,
   privilege changes, sensitive data access. Never log the secrets
   themselves.
7. **Fail closed.** When something is broken or ambiguous, deny.
   "Allow on error" is how breaches happen.

---

## Authentication

Authentication answers *who is this?* The answer must be trustworthy.

- **Use battle-tested libraries.** Do not implement password hashing,
  JWT parsing, OAuth flows, or signature verification by hand. Use the
  canonical library for your stack. Roll-your-own crypto is the path to
  a breach.
- **Hash passwords with a slow, memory-hard algorithm.** Argon2id (first
  choice), scrypt, or bcrypt. Never MD5, SHA1, SHA256, or unsalted hashes.
- **Tokens are bearer credentials.** Treat them like cash. Short-lived
  access tokens, longer-lived refresh tokens, revocable on the server.
- **Sessions:** secure, httpOnly, sameSite cookies for browser clients.
  Rotate session IDs on privilege change. Expire on inactivity.
- **MFA support belongs in the auth design from the start**, even if not
  enabled by default. Retrofitting it is painful.

### Common failures

- Verifying a JWT's signature with the algorithm field from the JWT itself
  (the `alg=none` and `alg=HS256` confusion attacks). Always enforce the
  expected algorithm server-side.
- Comparing tokens with `==` (timing attack). Use a constant-time
  comparison function.
- Storing tokens in localStorage and assuming XSS is impossible.
  Cookies with `httpOnly` are safer.

---

## Authorization

Authorization answers *may this actor do this thing?* Every protected
operation must ask this question, and the answer must come from a
single, central place — not scattered through controllers.

- **Default deny.** Permissions are explicit grants. Anything not
  granted is forbidden.
- **Enforce on the server.** Client-side hiding of a button is not
  authorization — it is presentation. The server must check.
- **Scope every query by the actor.** When fetching a user's data, the
  `WHERE user_id = ?` clause is not optional. Never trust the ID coming
  from the client to refer to the calling user's own resource — verify
  ownership.
- **Beware IDOR (Insecure Direct Object Reference).** `GET /orders/42`
  must check that order 42 belongs to (or is visible to) the caller.
  Sequential, guessable IDs make this attack trivial.

### Pattern: every protected endpoint

```
1. Resolve the actor from the auth credential (token, session)
2. Resolve the resource the action targets
3. Ask the central authorization function: may(actor, action, resource)?
4. If no — return 403/404 (404 if leakage of existence matters)
5. If yes — proceed
```

This is enforced at the framework level (middleware, decorator,
guard, etc.) so it cannot be silently omitted.

---

## Input Validation

Every input from outside the function is suspect. Validate at the boundary,
trust within.

- **Parse, don't validate.** Convert untrusted input into a typed,
  constrained domain object at the edge. After parsing, the rest of
  the code works with the typed value, not the raw input.
- **Whitelist what you accept.** "Reject these bad patterns" loses to
  attackers. "Accept only these good patterns" wins.
- **Limit sizes.** Strings, arrays, JSON depth, file uploads, URL
  lengths, header counts. Unbounded inputs become denial-of-service.
- **Encode at output, not at input.** HTML-encode when rendering to
  HTML; SQL-parameterize when querying SQL; JSON-encode when emitting
  JSON. The same input may need different encodings in different
  contexts.

### Injection — the family

All injection bugs share one cause: untrusted data interpreted as code.
The fixes are stack-specific but the rule is universal.

| Context | Wrong | Right |
|---|---|---|
| SQL | `db.exec("SELECT * FROM u WHERE id = " + id)` | Parameterized: `db.exec("...WHERE id = ?", [id])` |
| Shell | `os.system("ls " + path)` | Argument list: `subprocess.run(["ls", path], shell=False)` |
| HTML | `el.innerHTML = userInput` | Templated with auto-escaping, or `el.textContent` |
| File path | `open("data/" + name)` | Resolve and assert it stays under `data/` |
| Regex | `new RegExp(userPattern)` | Catastrophic backtracking — escape or refuse |
| LDAP, XPath, NoSQL | string concat | Use the driver's safe builder |

If your stack's library does not offer a safe path, that is the bug to
fix first.

---

## Secrets

Secrets are anything that grants access: API keys, database passwords,
private keys, OAuth client secrets, signing keys, encryption keys.

- **Never in source control.** Not in code, comments, config files,
  fixtures, tests, README, or commit history.
- **Never in logs.** Redact at the logger level. Audit logs themselves.
- **Never in error messages returned to clients.** "Database error:
  password authentication failed for user 'app' on host 10.0.1.5" is a
  gift to an attacker.
- **Environment variables or a secret manager.** Loaded once, at
  startup, by the adapter that uses them. Never read from the
  filesystem at request time.
- **Rotate** on schedule and on exposure. Plan rotation in the design,
  not after a leak.
- **Pre-commit hooks** that scan for secret patterns are mandatory. They
  catch the careless mistake the discipline alone will not.

---

## Data Protection

- **Encrypt sensitive data at rest.** Database-level encryption,
  column-level encryption for the most sensitive fields, encrypted
  backups.
- **TLS for everything in transit.** Internal services included. The
  network is hostile by default.
- **Minimize collection.** Data you do not have cannot be breached. Ask
  what data is needed, not what data might be useful.
- **Define retention.** Data has a lifecycle. Delete what you no longer
  need.
- **Tokenize where possible.** Card numbers, account numbers, anything
  that can be replaced with an opaque token usually should be.

---

## File Uploads

Hostile by default:

- Validate the type by inspecting bytes, not by trusting the extension
  or `Content-Type` header.
- Limit size strictly.
- Store outside the web root. Serve via a controller that authorizes.
- Never execute or interpret uploaded content. SVG is not safe — it
  contains scripts.
- For images, re-encode through a known library to strip metadata and
  embedded payloads.
- Scan with antivirus when the project's threat model warrants it.

---

## CSRF, CORS, and Cross-Origin Concerns

For any browser-facing API:

- **CSRF tokens** on every state-changing request, unless using
  `SameSite=strict` cookies and verifying the `Origin` header.
- **CORS** configured restrictively. `Access-Control-Allow-Origin: *`
  with credentials is a hole.
- **Content-Security-Policy** headers to constrain script execution.
- **HSTS** to prevent downgrade.

---

## Logging and Monitoring

- Log authentication outcomes (success and failure), authorization
  failures, sensitive data access, configuration changes, admin actions.
- Do not log: passwords, tokens, PII unless required and protected,
  payment data.
- Monitor: failed auth rates, authz denial rates, 5xx rates, latency
  outliers, unusual data egress.
- Alert on patterns, not single events.

---

## Dependencies

Third-party code is third-party risk:

- Pin versions.
- Run a vulnerability scanner (`npm audit`, `cargo audit`,
  `pip-audit`, `osv-scanner`) in CI.
- Subscribe to advisories for critical dependencies.
- Update on a regular cadence; do not let "later" become years.

---

## When to Refuse, Pause, and Escalate

Refuse the change, pause, and surface for human review when:

- Auth or authorization checks would be skipped, weakened, or
  centralized in a less-trusted place.
- Secrets would touch source control, logs, or error responses.
- User input would reach an interpreter without parameterization.
- A "temporary" exception is being added to the security baseline.
- The change handles data subject to specific regulation (PCI-DSS, HIPAA,
  GDPR, SOC2 controls) — a human must own the compliance decision.
- A library is being added that handles crypto, auth, or session
  management — verify it is the canonical choice.

---

## The Security Mantra

> **"Authenticate. Authorize. Validate. Parameterize. Encrypt. Log. Fail closed."**
