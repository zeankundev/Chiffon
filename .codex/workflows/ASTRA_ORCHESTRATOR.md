# Astra Multi-Agent Feature Workflow

Use this procedure for substantial feature requests. The root `AGENTS.md` and `.codex/AGENTS.md` remain authoritative.

## Model and team

- Astra is the lead orchestrator and should use an Astra model when available. If Astra is unavailable, explain the limitation before assigning work; do not silently change the requested model setup.
- Use 6–8 workers, preferably Terra or Sol, only when the requested feature has that many independent workstreams. Otherwise use the number that fits the work.
- Keep integration and final review with Astra. Workers should not change files outside their assigned ownership or create additional agents.
- Assign explicit model tiers when the agent interface supports it: Astra for the lead, Terra or Sol for workers. Ask the workers to operate as implementation agents, not coordinators.

## Orchestration checklist

1. Inspect the current working tree and the relevant implementation before assigning work. Preserve unrelated user edits.
2. State a concise plan and acceptance criteria. Call out dependencies and whether the work affects shared code, a specific architecture, drivers, IPC, capabilities, or docs.
3. Break the work into non-overlapping tasks. Assign each worker a distinct path set or subsystem and make interface dependencies explicit. For 6–8 agents, use a mix of bounded code work, documentation, and independent source review only where those are real tasks; do not invent work merely to fill seats.
4. Ask each worker to return a concise report using the task template in `.codex/AGENTS.md`.
5. Integrate sequentially where dependencies require it. Inspect the whole diff, reconcile interfaces, and confirm architecture docs match the code.
6. Verification boundary:
   - The sole permitted compile verification is the exact command `make ENV=milkos BUSYBOX=$HOME/busybox/busybox` on the user's WSL instance.
   - No other build/test/lint/format/emulator/boot/runtime command may be used to verify the work.
   - Workers must not directly verify behavior. Astra must not directly boot or interact with the system to verify behavior.
   - When behavioral evidence is needed, Astra instructs the human how to perform the check and what result to observe. Astra reports it as human-verified only after the human reports the result.
7. Report implementation, files changed, whether the allowed compile ran on WSL and its result, and any human checks still outstanding.
8. Do not mark the feature fully verified or complete while a required human check is outstanding. Wait for the human's reply, record the reported outcome, and then provide a concise changelog-ready list of new user-visible and technical changes.

## Human verification handoff

Use this format when a runtime or behavioral check is appropriate:

> **Human verification needed:** Please run/perform: `<specific user action>`.
> **Expected result:** `<observable outcome>`.
> **Report back:** `<specific output or pass/fail detail Astra needs>`.

Do not label the behavior verified before receiving the human's result.

## Reusable worker assignment

```text
You are a Terra/Sol implementation worker for Chiffon, reporting to Astra. Read root AGENTS.md and .codex/AGENTS.md. Work only on this task and the owned paths below. Do not spawn agents or edit outside those paths without reporting first.

Task: <one bounded outcome>
Owned paths: <explicit files/directories>
Dependencies: <interfaces/decisions or none>
Acceptance criteria: <observable conditions>

Preserve existing user changes and Chiffon's hybrid-kernel architecture. You may inspect source and reason about the implementation. Your only permitted verification command is exactly `make ENV=milkos BUSYBOX=$HOME/busybox/busybox`, and it may only run on the user's WSL instance. Do not use any other build, test, lint, format, emulator, boot, or runtime command. Do not directly verify behavior. If the exact WSL instance is unavailable, report the build as not run and make no substitute checks.

Return: summary, changed paths, key decisions, risks/dependencies, and whether the permitted WSL build ran with its result (or why it did not run).
```

## Changelog closeout

At closeout, group concise bullets under `New`, `Changed`, and `Fixed` only where applicable. Separate implementation facts from verification status. Include the exact build outcome; list human checks as pending until the human reports them. Do not invent user-facing changes or claim behavior was verified based only on source review or compilation.

