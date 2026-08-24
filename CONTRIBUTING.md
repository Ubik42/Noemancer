# Contributing to Noemancer

Noemancer is a pre-alpha engine with evolving public contracts. Small, evidence-backed changes are easier to review than broad rewrites.

## Before opening a change

1. Search existing issues and current documentation.
2. Keep game-specific rules in a project script or fixture rather than the engine Runtime.
3. Preserve the authority boundaries described in `docs/architecture.md`.
4. Prefer mature middleware behind an engine-owned adapter over introducing third-party types into public schemas.

For substantial architecture, persistence-schema or dependency changes, open a discussion before implementation.

## Build and verify

Use the smallest relevant target while iterating:

```powershell
./scripts/engine.ps1 check -Config Release `
  -Target <test-target> -TestRegex <ctest-regex>
```

Run the complete Release suite for changes that cross shared World, Runtime, build or persistence boundaries:

```powershell
./scripts/engine.ps1 test -Config Release
```

Use `-WithMcp` only when the Agent ABI or MCP adapter changes. Rendering changes should include a hidden capture or focused renderer evidence rather than a screenshot alone.

## Pull requests

Include:

- the problem and intended behavior;
- affected public contracts or schemas;
- tests and evidence actually run;
- known limitations or deferred follow-up;
- licensing and provenance for every new dependency or asset.

Do not commit generated build trees, local tools, credentials, proprietary game assets, downloaded papers or evidence that contains private machine paths.

## Style

- C++20; warnings are treated as errors.
- Keep persisted and Agent-facing identifiers stable and explicit.
- Make failure states structured and observable.
- Do not create a parallel source of truth for editor, scripting or Agent convenience.

By contributing, you agree that your contribution is licensed under the repository's Apache-2.0 license.
