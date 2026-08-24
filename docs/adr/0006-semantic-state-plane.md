# ADR 0006: Semantic State Plane before protocol-specific Agent tools

- Status: Accepted
- Date: 2026-08-18

## Context

Noemancer already exposes a command registry through direct JSON calls, CLI, and an MCP sidecar. The editor also has a demonstration semantic snapshot. Community engine integrations show that remote scene inspection, screenshots, runtime control, schema discovery, and large tool catalogs are becoming common. Adding more transports or tools alone would not make the engine model-native.

The current roadmap introduced the Engine Semantic Observation Graph after the scene, UI, asset, and rendering foundations. That order would allow each subsystem to establish incompatible identities, field meanings, revisions, and change formats before the graph exists.

## Decision

Noemancer will build a **Semantic State Plane** as an S1 foundation. It is a versioned, deterministic projection and change protocol over authoritative engine state; it is not a duplicate ECS or scene database.

The Semantic State Plane owns public contracts for:

1. stable object identity, semantic paths, schema references, and revisions;
2. semantic conventions including field meaning, unit, coordinate space, stability, sensitivity, and cost;
3. source anchors and typed cross-domain relationships;
4. filtered observation queries, consistent snapshots, pagination, and incremental deltas;
5. an Observation → Plan → Apply → Receipt mutation lifecycle;
6. manager identity, field-level intent, revision preconditions, conflict reporting, and explicit force semantics;
7. evidence and artifact links rather than embedding unbounded binary or verbose data.

The initial wire representation uses JSON Schema 2020-12 and JSON. It reuses URI and LSP-style source ranges, JSON Pointer for document fields, and JSON Patch where a low-level document patch is appropriate. Domain actions remain typed operations above JSON Patch.

Queries use a small selector and field-mask vocabulary with explicit depth, byte budget, cursor, and `sinceRevision`. We will not implement GraphQL or USD in S1.

MCP, CLI, JSONL event streams, the editor, accessibility, and tests are clients of this contract. Protocol adapters may map lifecycle and pagination but may not define new engine semantics.

## Consequences

- Stable identity, schema, conventions, and revision work moves ahead of Semantic UI and rendering.
- The existing Observation Graph becomes a family of read views over the Semantic State Plane, expanded to UI, viewport projection, render evidence, and visual attachments in S5.
- Every subsystem must define its semantic projection while it is built; AI support is no longer a late integration phase.
- Large tool catalogs are replaced by a small stable kernel plus discoverable, capability-gated profiles and higher-level planners.
- Plans are immutable artifacts bound to base revision and content hash. Apply revalidates them and cannot silently execute stale intent.
- Human and Agent edits can eventually use field manager metadata to explain conflicts instead of last-writer-wins overwrites.
- Canonical English identifiers remain stable while display labels are localizable.

The main cost is earlier investment in schema governance and deterministic projections. This is accepted because retrofitting those contracts after UI, asset, and renderer formats stabilize would be substantially more expensive.

## Rejected alternatives

- **MCP-first domain model:** rejected because MCP is a transport and its tool surface can change independently of engine semantics.
- **Serialize every internal object:** rejected because it leaks implementation types, creates unbounded context, and exposes unsafe state.
- **Pixel-only editor observation:** rejected because screenshots lose identity, binding, source, and exact values.
- **Text-only observation:** rejected because visual composition and rendering defects still require image and GPU evidence.
- **A second semantic world database:** rejected because dual authoritative state creates synchronization and correctness failures.

## Evidence

The design is informed by Bevy Remote Protocol discovery/watch methods, Godot text scenes and UIDs, Chrome DevTools DOM/accessibility/layout projections, OpenUSD paths and change notices, LSP source locations, OpenTelemetry semantic conventions, GraphQL/FieldMask selective reads, Terraform plan/apply, Kubernetes resource versions and managed fields, JSON Patch, and MCP resources/tools. The detailed comparison is recorded in [the model-native semantic substrate research report](../research/2026-model-native-semantic-substrate.zh-CN.md).
