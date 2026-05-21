---
name: prisma-ui
description: >
  How to consume the PRISMA UI library in a project that includes it as
  a git submodule. Defines: how PRISMA is wired into the workspace,
  token-first styling discipline, the project-side `ui-domain` package
  pattern for trading-domain or other domain primitives that are
  candidates for upstreaming into PRISMA, the component file structure,
  variant discipline via `cva`, and the rule that submodule files are
  never edited from the consuming project. Load before adding or
  modifying any UI component, any styling, any component that consumes
  PRISMA primitives, or anything in a `@<project>/ui-domain` package.
  Repo: git@github.com:tachyon-ops/prisma.git.
---

# PRISMA UI

> PRISMA is the shared UI library. It lives at `git@github.com:tachyon-ops/prisma.git`
> and is consumed in this project as a git submodule. Every visual atom,
> molecule, and organism this project renders is composed from PRISMA
> primitives — directly, or through a thin project-side wrapper that is
> a candidate for upstreaming into PRISMA itself.

---

## What PRISMA Is

PRISMA is a token-driven, dark-first, glassmorphic component library
following an Atoms → Molecules → Organisms → Layouts architecture. Its
core promises:

- **Substrate consciousness.** Every component knows its layer in the
  glass hierarchy. Blur intensity and opacity are tokens, not magic
  numbers.
- **Tokens over values.** No raw hex codes. No raw pixel values. Every
  visual property dereferences a token. This rule is enforced by lint
  and by review.
- **The Finance Color Contract.** Where finance contexts apply:
  `#00C896` is bullish, `#8B5CF6` is bearish. These resolve through
  PRISMA tokens (`--color-fin-bull`, `--color-fin-bear`); they are
  never written as literals in component code.
- **Variant discipline via `cva`.** Variants are declared with
  `class-variance-authority`, not by string concatenation or
  conditional `className` chains.
- **Per-component file structure.** A predictable layout that the
  agent can rely on.

---

## How PRISMA Is Wired In

PRISMA lives at `git@github.com:tachyon-ops/prisma.git` and is included
in this project as a git submodule. The wiring (record concrete paths
in `codebase-map`):

```
<repo-root>/
├── .gitmodules                     ← declares the submodule
├── vendor/prisma/                  ← submodule checkout (path may differ; see codebase-map)
│   ├── packages/ui/                ← the @prisma/ui package source
│   └── ...
├── pnpm-workspace.yaml             ← lists vendor/prisma/packages/ui as a workspace member
└── apps/<app>/
    ├── package.json                ← declares "@prisma/ui": "workspace:*"
    └── next.config.ts              ← includes "@prisma/ui" in transpilePackages
```

The discipline:

1. **Workspace member, not registry dependency.** PRISMA is resolved
   in-tree through the workspace protocol, not from the npm registry.
2. **Pinned commit.** The submodule is pinned to a specific commit in
   the parent repo's history. CI clones with
   `git submodule update --init --recursive`.
3. **`transpilePackages`.** Bundler configs (Next.js, Vite, etc.) must
   include `@prisma/ui` in their transpile list — otherwise the
   workspace source is shipped untranspiled.

---

## The Submodule Rule (Non-Negotiable)

**The agent does not edit files inside the PRISMA submodule from this
repository.** This is the rule from the `monorepo` skill, applied
specifically to PRISMA.

If a PRISMA component needs a fix, a new variant, or a new prop:

```
1. Surface the need in the task summary, with a clear description of
   the change required.
2. The change happens in the PRISMA repository, in its own commit, by
   the human or a separate agent task scoped to that repo.
3. The parent repo's submodule pin is updated, on explicit human
   instruction, in a separate commit.
4. Consumers of the changed PRISMA API are updated in another commit.
```

The agent does **not** bump the submodule pin without explicit human
instruction.

---

## The `ui-domain` Pattern

This project will have UI needs that are too domain-specific to live
in PRISMA today, but may belong there tomorrow. The pattern for these
is a project-side package that follows PRISMA conventions exactly.

Typical layout:

```
packages/<project>-ui-domain/        ← e.g. @<project>/ui-domain
├── package.json
├── tsconfig.json
├── README.md
└── src/
    ├── index.ts
    ├── utils/
    │   └── cn.ts                    ← class-name composer (clsx wrapper)
    └── primitives/
        └── <ComponentName>/
            ├── <ComponentName>.tsx
            ├── <ComponentName>.types.ts
            ├── <ComponentName>.stories.tsx
            ├── <ComponentName>.test.tsx
            └── index.ts
```

`@<project>/ui-domain` declares `@prisma/ui` as a `workspace:*`
dependency and composes its primitives from PRISMA atoms and molecules.
The package's purpose is to be a holding area for components that:

- Use the same tokens as PRISMA.
- Use the same `cva` variant patterns as PRISMA.
- Use the same file structure as PRISMA.
- Are written with the explicit intent of being upstreamable to PRISMA
  when the API stabilises and the use case is general enough.

When a component in `ui-domain` matures, the upstreaming work is:

```
1. Move the component into the PRISMA repository, in the appropriate
   atoms/molecules/organisms tier.
2. Add its Storybook stories there.
3. Bump the submodule pin in the parent repo (human instruction).
4. Replace the import in the consuming code: from @<project>/ui-domain
   to @prisma/ui.
5. Remove the component from @<project>/ui-domain.
```

This pattern means the agent has a clear answer to "where does this
new component go": if it is general, propose it for PRISMA; if it is
project-specific but PRISMA-shaped, put it in `ui-domain`; if it is
truly app-specific, it lives inside the app.

---

## Component File Structure

Every component — in PRISMA, in `ui-domain`, and (where it warrants
the ceremony) in the app — follows this layout:

```
<ComponentName>/
├── <ComponentName>.tsx           ← the React component
├── <ComponentName>.types.ts      ← public prop types and variants
├── <ComponentName>.stories.tsx   ← Storybook stories (at least Default + AllStates + Playground)
├── <ComponentName>.test.tsx      ← render + a11y tests
├── index.ts                      ← barrel re-export
└── README.md                     ← (optional) prop docs and token usage
```

When the component is in `ui-domain` and intended for upstreaming, the
stories file carries a header marker:

```tsx
// COPY TO PRISMA WHEN UPSTREAMED.
```

---

## Token Discipline (Non-Negotiable)

These rules are enforced by lint and by review. The agent treats them
as hard rules:

1. **No raw hex codes** in component code. Colors come from tokens:
   `var(--color-bg-glass-1)`, Tailwind classes that resolve to tokens,
   or PRISMA-exported color constants.
2. **No raw pixel values** for spacing, radii, or typography. Use
   PRISMA's scale: `var(--space-*)`, `var(--radius-*)`,
   `var(--font-size-*)`. The exception is one-px borders and similar
   atomic primitives — and even those typically have a token.
3. **No inline `style={{ color: '...' }}`** with literal values. Use
   classes that resolve through tokens.
4. **The Finance Color Contract.** When rendering bullish/bearish
   states, use the finance tokens. Never `'#00C896'` or `'#8B5CF6'`
   in component source.

If a needed token does not exist in PRISMA, that is a PRISMA change —
surface it; do not work around with a literal.

---

## Variant Discipline

Component variants are declared with `class-variance-authority` (`cva`),
not with chains of conditional classes. The pattern PRISMA follows
(and `ui-domain` mirrors):

```ts
// Button.types.ts
import { type VariantProps, cva } from 'class-variance-authority';

export const buttonVariants = cva(
  // base classes — tokens only
  'inline-flex items-center justify-center rounded-[var(--radius-md)] transition',
  {
    variants: {
      tone: {
        primary:   'bg-[var(--color-accent)] text-[var(--color-on-accent)]',
        glass:     'bg-[var(--color-bg-glass-2)] backdrop-blur-[var(--blur-2)]',
        ghost:     'bg-transparent hover:bg-[var(--color-bg-glass-1)]',
      },
      size: {
        sm: 'h-8 px-[var(--space-3)] text-[var(--font-size-sm)]',
        md: 'h-10 px-[var(--space-4)] text-[var(--font-size-md)]',
        lg: 'h-12 px-[var(--space-5)] text-[var(--font-size-lg)]',
      },
    },
    defaultVariants: { tone: 'primary', size: 'md' },
  },
);

export type ButtonVariants = VariantProps<typeof buttonVariants>;
```

```tsx
// Button.tsx
import { buttonVariants, type ButtonVariants } from './Button.types';
import { cn } from '../../utils/cn';

export interface ButtonProps
  extends React.ButtonHTMLAttributes<HTMLButtonElement>,
    ButtonVariants {
  readonly leadingIcon?: React.ReactNode;
}

export function Button({
  className,
  tone,
  size,
  leadingIcon,
  children,
  ...rest
}: ButtonProps): React.ReactElement {
  return (
    <button className={cn(buttonVariants({ tone, size }), className)} {...rest}>
      {leadingIcon}
      {children}
    </button>
  );
}
```

The `cn` utility composes classes via `clsx`. It is the only class
composer used.

---

## Theming

PRISMA exposes themes via a `data-theme` attribute on the document
root, with a corresponding set of CSS custom properties. Switching
themes is a one-line operation:

```tsx
document.documentElement.setAttribute('data-theme', 'dark-finance');
```

Themes the project may select (PRISMA-provided): `dark-luxe`,
`dark-finance`, `dark-terminal`, `light-pearl`, `editorial`,
`high-contrast`. The exact set lives in PRISMA's documentation; the
agent reads `codebase-map` for which themes this project enables.

The agent does **not** invent new themes in consuming projects. New
themes are a PRISMA concern.

---

## Storybook in the Consuming Project

If the project ships its own Storybook (separate from PRISMA's), it
imports from `@prisma/ui` and `@<project>/ui-domain` for the stories
it owns. PRISMA's stories live in the PRISMA repo and ship through
the PRISMA submodule's own Storybook target.

If a `ui-domain` component carries a `// COPY TO PRISMA WHEN UPSTREAMED.`
marker, its stories file is structured to be moved into PRISMA's
Storybook as-is when the upstreaming happens.

---

## Anti-Patterns to Refuse

- **Raw hex codes or pixel values** in any component or style. Refuse;
  use tokens.
- **Inline styles with literal colors.** Refuse.
- **String concatenation for variants.** Use `cva`.
- **Importing from `@prisma/ui/src/...`** or any deep path inside the
  package. Use the public entry point only.
- **Editing files inside the PRISMA submodule** from this repo.
  Surface the need; the work happens in the PRISMA repo.
- **Bumping the submodule pin without explicit human instruction.**
- **Adding a UI primitive directly inside an app** when its shape is
  general enough to live in `ui-domain` or PRISMA.
- **Forking PRISMA components** by copy-pasting their source into the
  project. If the fork is needed, it is either a `ui-domain` extension
  or a PRISMA change. Never a copy.

---

## What the Agent Must Do

- Read `codebase-map` for the exact submodule path, the workspace
  layout, and the list of themes this project uses.
- Before writing a component: decide where it goes — PRISMA (propose,
  do not edit), `ui-domain`, or app-local. Justify the placement.
- Use PRISMA primitives and tokens for every visual concern. No raw
  literals.
- Follow the file structure (`.tsx`, `.types.ts`, `.stories.tsx`,
  `.test.tsx`, `index.ts`) for every new component.
- Declare variants via `cva`.
- When a PRISMA change is needed, surface it. Do not work around it.

---

## When to Refuse and Pause

Refuse a change, and surface, when:

- A file inside the PRISMA submodule would be edited from this repo.
- The submodule pin would be bumped without explicit instruction.
- A raw hex code or pixel value would be introduced in a component.
- A component is being copy-pasted from PRISMA rather than consumed.
- A new theme would be defined outside PRISMA.
- A UI primitive that should live in `ui-domain` is being added
  directly inside an app.

---

## The PRISMA Mantra

> **"Tokens over values. Compose from PRISMA. Stage in `ui-domain`. Upstream when ready. The submodule is read-only here."**
