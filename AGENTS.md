# Repository instructions

- Build and test through `scripts/engine.ps1`; do not invent alternate commands without documenting them.
- Read `docs/current-state.json`, `docs/architecture.md`, and the current-frontier section of `docs/development-plan.zh-CN.md` before selecting long-term work. `docs/research/` is historical and non-authoritative.
- Keep the actual target direction: `noemancer_engine <- noemancer_editor <- noemancer runtime`; MCP and managed projects consume stable engine contracts without owning native domain behavior.
- Third-party types must not appear in persisted scene formats or public RPC schemas.
- Prefer explicit ownership and plain data over macros, hidden registration, and global service locators.
- Every public command supports structured output and returns a stable non-zero exit code on failure.
- Any runtime mutation exposed to an agent must eventually support dry-run, revision checks, transactions, and undo.
- Generated files belong under `generated/` and must name their source schema and generation command.
- Use risk-based tests. Add or update a focused test for bug fixes, public contracts, persistence, concurrency, ownership, transactions, parsers, or other behavior whose regression would be costly. Do not mechanically add a new case for comments, documentation, renames, wiring-only changes, or behavior already covered by a nearby test.
- When architecture or priorities change, rewrite or remove obsolete statements in `docs/architecture.md`, `docs/development-plan.zh-CN.md`, and `docs/first-acceptance-status.zh-CN.md` in the same commit. Do not leave contradictory “next step” text and append a newer claim below it.
- After documentation governance changes, run `scripts/audit-governance.ps1`; it is a lightweight document check, not an engine test gate.

## Fast multi-agent development

- Long-horizon engine work uses a Sol orchestrator plus bounded `luna_worker` agents. The orchestrator owns architecture, priority, integration, authoritative documents and final verification; workers receive narrowly scoped, independently completable lanes.
- Prefer the user-configured `luna_worker` profile for focused implementation, repository exploration, mechanical migration, fixture construction, targeted test work and evidence gathering. Do not delegate architecture-wide decisions or an indivisible critical path merely to create activity.
- Parallelism is primarily for implementation throughput. When two or more write sets can be isolated, assign workers concrete code, test, fixture or evidence-script deliverables; do not fill every available lane with read-only audits while the user is waiting for module completion. An audit lane is appropriate only when it resolves a specific uncertainty needed by an implementation lane, and its findings should normally be converted into code or acceptance evidence in the same batch.
- Name and brief workers by the deliverable they own, not with a generic `audit` label when the requested outcome is development. For a three-worker batch with separable work, prefer multiple independent writer lanes plus at most one bounded research/review lane. Read-only fallback remains valid when safe write ownership genuinely cannot be isolated; it is not the default substitute for parallel development.
- The user has explicitly granted standing authorization for proactive subagent delegation in this repository. There is no "only when the user explicitly asks in this turn" gate: this repository instruction is the continuing explicit request to use subagents whenever they materially accelerate the work. Subagent use is the orchestrator's default scheduling decision, not a per-turn permission gate; do not wait for the user to name or re-authorize subagents. When a batch contains independent, non-overlapping lanes, proactively use up to three `luna_worker` agents in parallel; keep the critical path local only when subdivision would add coordination cost.
- Default workers to read-only audit/evidence work when write ownership cannot be isolated. Multiple writers must never edit the same files or authoritative state concurrently; the Sol orchestrator alone integrates cross-lane changes, updates canonical documents and commits the batch.
- Run up to three worker lanes when their write sets are disjoint. Never assign multiple writers to the same files or subsystem authority. Nested delegation also does not require per-turn user authorization; a worker may use an available slot when its brief contains a genuinely independent nested lane, while the Sol orchestrator remains responsible for avoiding oversubscription and overlapping ownership.
- A worker brief must name scope, allowed files or ownership boundary, acceptance evidence, and forbidden expansion. Workers return changed files, verification performed, risks and decisions for the orchestrator.
- The orchestrator reviews and integrates worker output against the real tree. A worker report is evidence, not authority, and does not by itself update current status.
- Shared CMake/dependency declarations, public schemas, canonical documents and concentration points such as `world.cpp`, `scene_renderer.cpp`, `command_registry.cpp` and `editor_ui.cpp` default to a single Sol-owned writer unless ownership is explicitly partitioned.

## Codex Goal / Ralph Loop protocol

- `docs/current-state.json.currentFrontier` is the durable queue. Select the first unblocked item, then take the largest coherent batch that preserves one reviewable subsystem boundary.
- Each loop is: orient from the canonical documents -> inspect the real execution path -> split only independent lanes -> implement a coherent batch -> integrate/review -> run batch-level focused verification -> rewrite canonical status -> continue to the next batch.
- Accumulate related edits before testing; do not compile after every file or add tests for every implementation detail. Do not cross a subsystem batch boundary with known compile errors, ownership ambiguity, or an unverified high-risk contract.
- Continue without asking for routine choices while an in-scope safe path exists. Stop only for a genuine product decision, missing authority, repeated external blocker, milestone acceptance, or an explicit user pause.
- Long-term continuation is driven by an active Codex `/goal`. It is not a scheduled task or a shell script that recursively prompts Codex. The repository supplies the durable queue and batch rules; the Goal resumes them and leaves the tree recoverable at every batch boundary.

## Verification cadence

- Worker/inner loop: inspect and edit freely within a bounded lane; compile early only when an uncertain API or dependency makes continued edits speculative. Documentation-only edits need no build.
- Integrated batch: compile affected targets and run the smallest existing relevant test set after a coherent subsystem slice, not after each file.
- Batch boundary: run focused subsystem tests and one directly relevant executable/tool probe.
- Full `scripts/engine.ps1 test`: only at milestone/acceptance boundaries, before release, or after cross-cutting changes to public schemas, shared World/runtime infrastructure, build configuration, or dependencies. Add `-WithMcp` only for command registry, Agent ABI, MCP adapter, or milestone changes.
- Hidden GPU capture: only after renderer/shader/GPU-resource changes or when producing visual acceptance evidence. Dual D3D12/Vulkan capture is not a generic engine gate.
- Prefer a few durable behavioral contracts over one test per implementation detail.

At a milestone or acceptance boundary, canonical full verification remains:

```powershell
./scripts/engine.ps1 configure
./scripts/engine.ps1 build
./scripts/engine.ps1 test
./scripts/engine.ps1 test -WithMcp
```
