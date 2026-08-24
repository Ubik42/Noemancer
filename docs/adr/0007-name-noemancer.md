# ADR 0007: Product name is Noemancer

## Status

Accepted, 2026-08-18.

## Context

The bootstrap name “SemaForge” described semantic construction but sounded generic and did not carry the engine's science-fiction identity. Renaming is cheapest before public releases, package publication, serialized project formats, and third-party plugins exist.

## Decision

The product, editor, executable, CMake targets, C++ namespace, schemas, MCP package, and command examples use **Noemancer** / `noemancer`.

The name is an original compound:

- *noema*: the content or object as understood by a mind;
- *-mancer*: used here in the modern fictional sense of one who shapes or works through a domain.

It therefore means “a system that shapes worlds into directly understandable meaning,” matching the Semantic State Plane and Engine Semantic Observation Graph. Its rhythm is a deliberate literary nod to William Gibson's *Neuromancer*, winner of the 1985 Hugo Award, without copying the book title, a character, or an in-world proprietary term.

A preliminary web and GitHub search on 2026-08-18 found no same-name game engine or software product. This is a naming screen, not a legal trademark clearance; perform formal jurisdiction-specific checks before public commercial release.

## Consequences

- New user-visible and machine-visible identifiers must not use the former name or `aine` abbreviation.
- Stable serialized identifiers will use the `noemancer` namespace before any compatibility promise is made.
- Literary inspiration is explained as provenance, not presented as affiliation or endorsement.

