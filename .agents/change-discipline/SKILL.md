---
name: change-discipline
description: >
  How to keep changes minimal, bounded, and free of duplication. Load
  before editing any file, when tempted to refactor unrelated code, when
  introducing a new function or component, or when the task feels like
  it is growing. Stack-agnostic.
---

# Change Discipline

> One concern per change. One reason to revert per commit. Drive-by
> improvements are a different task.

---

## The Three Disciplines

1. **Minimal.** Make the smallest change that satisfies the requirement.
2. **Bounded.** Touch only what the task names. Mark everything else.
3. **Non-duplicating.** Search before you create. Reuse before you write.

These are not separate ideas. A bounded change cannot be duplicating; a
minimal change cannot be drive-by refactoring; a non-duplicating change
is, by construction, smaller.

---

## Minimal

The smallest change is the one a reviewer can hold in their head.

| Symptom | Cause | Fix |
|---|---|---|
| Diff is hundreds of lines for a small request | Reformatting, reordering, renaming alongside the change | Revert the unrelated parts; commit them separately later |
| Three new files added for a one-line behaviour fix | Premature abstraction | Inline the change; extract when duplication forces it |
| Imports churned by an auto-formatter | Editor settings | Configure the editor; do not include in the diff |
| "While I was here, I noticed..." | Scope creep | Add a `NOTE:` comment; report it; move on |

Read the diff before submitting it. Ask: *does every changed line serve
the stated task?* If not, the answer is to remove those lines from the
diff, not to justify them.

---

## Bounded

A task has a perimeter. Inside it, you act. Outside it, you observe and
report.

When you spot a problem outside the perimeter:

```
1. Mark it where it lives:
   // NOTE: [problem] — out of scope for [current task], see [issue or TODO]
2. Report it in your output summary's "Flags for human review"
3. Move on
```

When you spot a problem *blocking* the task:

```
1. Pause: "AGENT PAUSE: cannot complete [task] because [blocker]"
2. Wait for instruction
```

Never silently expand scope. Even good changes hidden in unrelated
diffs are bad changes — they break review, they pollute history, they
hide their own intent.

---

## Bounded — by Layer

A change should not cross architectural layers unless the task says so.

- A bug fix in a pure function: edit the function, add a test.
- A bug fix in an adapter: edit the adapter, add an integration test.
- A bug fix that turns out to be a domain rule error: stop. Surface it.
  Domain changes are a different conversation from infrastructure changes.

If you find yourself editing three layers to ship one feature, the
design may be at fault — flag it. Do not paper over it with a sprawling
diff.

---

## Don't Repeat Yourself — But Wait

DRY is a goal, not a reflex. Premature abstraction is as costly as
duplication, and harder to undo.

The discipline is:

```
1. Search before you write. Is there an existing function, type, or
   component that does this (or almost this)?
2. If yes — use it. Extend if it needs one more case. Refuse to write
   a parallel implementation.
3. If no — write the thing inline. Do not extract yet.
4. When a second site needs the same logic — copy or extract, depending
   on whether the shape is now clear.
5. By the third site, extract. The pattern is real.
```

**The rule of three:** one is a fact, two is a coincidence, three is a
pattern. Extracting at two locks in a shape that may not hold.

---

## When You Find Duplication

```
1. Confirm it really is the same logic. "Looks similar" is not the same
   as "is the same." Subtle differences matter.
2. Identify the smallest extractable unit. A helper function, a single
   constant, a shared type. Not a "BaseFooManagerAbstract".
3. Place the extracted thing one layer below the duplicating consumers.
4. Update all consumers in the same change. Never leave a half-migration.
```

If the duplication crosses package boundaries and the extracted thing
would create a new cross-package dependency, pause. The architectural
cost may outweigh the duplication. Surface the trade-off.

---

## Refactoring vs. Feature Work

These are different tasks. Mixing them produces diffs that are neither
reviewable nor revertable.

- **Feature work:** changes behaviour. The tests prove the new behaviour.
- **Refactor:** preserves behaviour. The tests should pass unchanged.

If a feature requires a refactor first, do the refactor as its own
change, then the feature. Two diffs, two commits, two reviews.

The exception: a refactor so tightly entangled with the feature that
separating them would be misleading. Rare. When it happens, the commit
message says so explicitly.

---

## Commit Discipline

One logical change per commit. The commit message names the *what* and
the *why*. The diff shows the *how*.

```
feat(orders): apply tiered discount to multi-item carts

Carts with >5 items now receive a 10% discount applied to the cart
subtotal before tax. Implements FR-204.

The pricing logic was already pure; only the rule function changed.
No I/O surface changes.
```

If you cannot write a commit message that fits one screen, the commit
is doing too much.

---

## When to Refuse

Refuse a change, and surface the conflict, when:

- The diff includes lines unrelated to the stated task.
- A "small fix" requires a structural refactor.
- The change duplicates existing logic the agent failed to find.
- A reasonable reviewer would ask "why is this here?" about any part of
  the diff.

---

## The Change-Discipline Mantra

> **"Smallest change that works. One concern per diff. Search before you write. Mark what you do not touch."**
