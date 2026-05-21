---
name: codebase-map
description: >
  Project-specific orientation: where every kind of thing lives in this
  codebase. This skill is a TEMPLATE — each project fills it in with
  concrete paths, package names, and conventions. Load at the start of
  every session to know where to look and where to put things.
---

# Codebase Map

> *This file is a template. When dropped into a project, replace the
> placeholders with the project's actual structure. Keep this file
> short, accurate, and current — when the structure changes, this file
> changes in the same commit.*

---

## Project Identity

- **Name:** `<project name>`
- **Purpose (one sentence):** `<what this codebase exists to do>`
- **Primary stack:** `<language(s), framework(s), runtime>`
- **Repository layout:** `<monorepo / polyrepo / hybrid>`

### If monorepo, also:

- **Package manager:** `<pnpm / cargo / uv / cmake / bazel / nx / ...>`
- **Workspace manifest:** `<pnpm-workspace.yaml / Cargo.toml [workspace] / WORKSPACE / ...>`
- **Build orchestrator:** `<turbo / nx / bazel / cargo / make / ...>`
- **Versioning mode:** `<single-version policy / independent + changesets / ...>`
- **Workspace glob(s):** `<apps/*, packages/*, services/*, vendor/prisma/packages/* ...>`

See `monorepo/SKILL.md` for the discipline that applies inside the
workspace.

---

## Submodules

*List every git submodule and the discipline that governs it. The
agent verifies submodules are checked out before working on code that
depends on them.*

| Submodule | Path | Purpose | Governing skill |
|---|---|---|---|
| Prisma UI | `vendor/prisma` | Shared UI library across all org projects | `prisma-ui` |
| `<other>` | `<path>` | `<purpose>` | `<skill, if applicable>` |

```bash
# Initial clone with submodules
git clone --recurse-submodules <project-url>

# Or after a plain clone
git submodule update --init --recursive
```

Submodule discipline lives in `monorepo/SKILL.md` ("Git Submodules and
Vendored Code") and, for Prisma specifically, in `prisma-ui/SKILL.md`.

---

## Top-Level Structure

```
<project-root>/
├── <packages or src dir>/        ← <one-line purpose>
├── <apps dir, if any>/           ← <one-line purpose>
├── <libs dir, if any>/           ← <one-line purpose>
├── <tests dir, if separated>/    ← <one-line purpose>
├── vendor/                       ← git submodules (e.g. vendor/prisma)
├── tools/                        ← scripts, generators, repo-local CLIs
├── docs/                         ← project documentation
│   └── adr/                      ← architectural decisions (see decisions-log)
├── .agent/skills/                ← agent skills (this file + others)
└── <tooling configs>             ← package manager, build, lint, format
```

*Replace the above with the actual repository tree. Comment each
top-level directory in one line.*

---

## Where Things Live

A table the agent can scan in two seconds to know where new code goes.
Fill in for the project.

| Kind of thing | Where it lives | Naming convention |
|---|---|---|
| Domain entities and types | `<path>` | `<convention>` |
| Pure business logic | `<path>` | `<convention>` |
| Ports (interfaces for I/O) | `<path>` | `<convention>` |
| Adapters (Port implementations) | `<path>` | `<convention>` |
| HTTP / API endpoints | `<path>` | `<convention>` |
| Background jobs / workers | `<path>` | `<convention>` |
| Database migrations | `<path>` | `<convention>` |
| Configuration | `<path>` | `<convention>` |
| Shared utilities | `<path>` | `<convention>` |
| Frontend components | `<path>` | `<convention>` |
| Frontend pages / routes | `<path>` | `<convention>` |
| Frontend state / hooks | `<path>` | `<convention>` |
| Unit tests | `<path>` | `<convention>` |
| Integration tests | `<path>` | `<convention>` |
| End-to-end tests | `<path>` | `<convention>` |

---

## Naming Conventions

*Fill in the project's actual conventions. Examples:*

| Thing | Convention | Example |
|---|---|---|
| Package / module | `<...>` | `<...>` |
| Public function | `<...>` | `<...>` |
| Internal function | `<...>` | `<...>` |
| Type / class | `<...>` | `<...>` |
| Constant | `<...>` | `<...>` |
| File | `<...>` | `<...>` |
| Test file | `<...>` | `<...>` |
| Environment variable | `<...>` | `<...>` |

---

## Package / Module Dependency Direction

*Describe the allowed dependency flow. Example layering:*

```
apps/* and services/*       ← may depend on everything below
    ↓
domain/*                    ← may depend on ports/*; may NOT depend on infra/*
    ↓
ports/*                     ← interfaces only; no dependencies on infra/*
    ↑ (adapters implement ports)
infra/*                     ← may depend on ports/* (to implement them) and on shared/*
    ↓
shared/*                    ← pure, dependency-free utilities
```

*Reject changes that violate this direction. See the `architecture`
skill.*

---

## Build, Run, Test

The exact commands an agent needs. No prose, just commands.

In a monorepo, **default to scoped or affected commands** in the inner
loop. Whole-repo commands belong in CI. See `monorepo/SKILL.md` →
"Scoped Commands".

```bash
# Install
<install command>

# Run dev / local — one app
<dev command for one app>

# Tests
<test command, single package>    # one package only — fastest, used during edits
<test command, affected>          # affected by current branch — used before declaring done
<test command, all>               # entire repo — used in CI

# Lint and format
<lint command, scoped>
<lint command, all>
<format command>

# Type-check (if separate from build)
<type-check command, scoped>
<type-check command, all>

# Submodule init (if any submodules are present)
git submodule update --init --recursive
```

---

## Environment

- Required environment variables — list them in `.env.example`, never
  in code or in this file.
- Secrets handled per the `security-fundamentals` skill.
- Local dependencies (database, cache, message bus) — `<docker-compose
  file or equivalent>`.

---

## Key Documents to Read

*List the project's most important reading material. The agent should
read these in order on session start.*

1. This file (`codebase-map`)
2. `agents-rules` skill — the constitution
3. `<project>-stack` skill — stack and domain rules (if it exists)
4. `monorepo` skill — if this project is a monorepo
5. `prisma-ui` skill — if this project consumes the PRISMA submodule
6. Other library skills relevant to the task (`architecture`,
   `api-design`, `functional-core`, `change-discipline`,
   `third-party-integrations`, `security-fundamentals`, `testing`,
   `errors-and-observability`)
7. `docs/adr/` — relevant decision records

---

## Update Discipline

*This file is a contract with the agent. When it lies, the agent
hallucinates. Therefore:*

- When a directory is added, renamed, or removed, this file is updated
  in the same commit.
- When a naming convention changes, this file is updated in the same
  commit.
- When the dependency direction changes, that is an architectural
  decision — record it in an ADR, then update this file.

---

## The Codebase-Map Mantra

> **"Tell the agent where things live. Keep it true. Update in the same commit. A lying map is worse than no map."**
