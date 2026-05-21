---
name: decisions-log
description: >
  Architectural Decision Records (ADRs) — the process and template for
  recording why a non-trivial decision was made. Load before making any
  decision that affects multiple packages, changes a public API, picks
  a new vendor or technology, or that a future maintainer would
  reasonably ask "why?" about. Load also before contradicting an
  existing decision.
---

# Decisions Log

> The codebase records its decisions, not just its outcomes. A future
> maintainer (or future agent) should be able to read why a thing was
> done before asking to undo it.

---

## Why ADRs

Architectural Decision Records exist because code answers *what* and
*how*, but rarely *why*. A year later, someone — possibly an AI agent
— sees a design choice and wonders if it was deliberate. Without a
record, the safest assumption is that it was an accident and can be
changed. With a record, the constraint is visible and respected.

ADRs are short. They are not documentation. They are minutes — a
durable note of a decision and its context.

---

## When to Write One

Write an ADR when:

- A choice affects more than one package.
- A public API takes a specific shape for non-obvious reasons.
- A vendor or library is selected from multiple plausible options.
- A pattern is adopted (or forbidden) across the codebase.
- A trade-off is made that future maintainers will question.
- A previous ADR is superseded or amended.

Do not write one when:

- The choice is fully described by an existing standard or convention.
- The decision is local to a single function.
- It is implementation detail with no consequences for other code.

The question to ask: *would someone six months from now reasonably ask
why?* If yes, write the ADR.

---

## When to Read Them

Read ADRs:

- Before designing anything that touches their subject.
- When an existing pattern in the code looks "wrong" — the ADR may
  explain.
- Before proposing to change or remove a constrained pattern.

If you are about to contradict an existing ADR, pause and surface it
to the human. ADRs are not unchangeable — they are explicitly
revisable — but the revision is a decision in itself.

---

## Storage

ADRs live in the repository, versioned with the code. Note that the `docs/adr/` directory is virtual/planned and should be created on demand when writing the first ADR.

A typical location:

```
docs/adr/
  0001-database-engine.md
  0002-auth-strategy.md
  0003-frontend-framework.md
  0004-event-bus.md
```

Numbering is sequential and never reused. Filenames include the number and a short slug.

---

## Template

Every ADR follows this structure. Keep it short — usually under one
page.

```markdown
# ADR-NNNN: Short Title

- **Status:** Proposed | Accepted | Superseded by ADR-MMMM | Deprecated
- **Date:** YYYY-MM-DD
- **Deciders:** Decider Name or Role

## Context

What problem are we solving? What forces, constraints, and prior
decisions shape it? Two to four paragraphs. Be specific. Name the
trade-offs being made.

## Decision

The decision, stated plainly. One paragraph. Should be unambiguous
enough that the implementing engineer (or agent) does not need to
re-derive it.

## Alternatives Considered

What other options were weighed, and why were they rejected? Three to
five bullets, each one or two sentences. Be honest about the trade-offs
— "we picked X because we are familiar with it" is a valid reason if
it is the real reason.

## Consequences

What changes because of this decision? What new constraints does the
codebase now operate under? What follow-up work does it imply?

### Positive
- ...

### Negative
- ...

### Neutral
- ...

## References

- Links to relevant docs, discussions, issues, or external resources.
```

---

## Status Lifecycle

- **Proposed.** Written but not yet adopted. Open for discussion.
- **Accepted.** Adopted. The code reflects, or will reflect, the
  decision.
- **Superseded.** A later ADR replaces this one. The header points to
  the successor. The original text is preserved — history matters.
- **Deprecated.** No longer applicable but not formally replaced.
  Explain why in a final paragraph.

Never delete or edit an accepted ADR to change the decision. To change
the decision, write a new ADR that supersedes it.

---

## Example

```markdown
# ADR-0002: Authentication via OAuth2 with PKCE

- **Status:** Accepted
- **Date:** 2026-03-04
- **Deciders:** core team

## Context

The application has a browser frontend, a mobile app planned for Q3,
and a public API. We need a single authentication model that works for
all three without splitting auth flows. Session cookies do not work
for the mobile app; bearer tokens stored in localStorage do not work
for browsers given our XSS threat model.

## Decision

Use OAuth2 with PKCE for all clients. Browser frontend stores tokens
in httpOnly secure cookies set by a same-origin auth proxy. Mobile
app uses native secure storage. Public API consumers use the standard
OAuth2 client-credentials flow.

## Alternatives Considered

- **Session cookies only.** Breaks for mobile and API.
- **Custom token scheme.** Reinventing wheels with security
  consequences. Rejected.
- **Auth0 / Okta.** Considered. Rejected for cost and data
  residency requirements; revisit if scale demands it.

## Consequences

### Positive
- One auth model across all clients.
- Mobile and API onboarding are straightforward.

### Negative
- Same-origin proxy adds an operational component.
- Token refresh flow complexity in the SPA.

### Neutral
- Auth library choice constrained to OAuth2-compliant ones.

## References
- IETF RFC 7636 (PKCE)
- Internal threat model doc /docs/security/threat-model.md
```

---

## What an Agent Must Do

- Before making a non-trivial architectural choice, search `docs/adr/` for relevant prior decisions (if the directory exists).
- If a relevant ADR exists, follow it. If you would contradict it,
  pause and surface the conflict.
- If no ADR covers the decision and the decision is consequential,
  draft one — even briefly (creating the `docs/adr/` folder if it does not yet exist) — and ask the human to ratify it before
  implementing.
- When writing an ADR, follow the template. Keep it short. Be honest
  about the trade-offs.

---

## When to Refuse

Refuse a change, and surface, when:

- The change contradicts an Accepted ADR without superseding it.
- A consequential decision is being made silently in code that should
  have an ADR first.

---

## The Decisions-Log Mantra

> **"Record the why. Read before you contradict. Supersede deliberately. Honest trade-offs only."**
