# Astra Multi-Agent Feature Workflow

Use this procedure for substantial feature requests. The root `AGENTS.md` and `.codex/AGENTS.md` remain authoritative.

## Model and team

- Astra is the lead orchestrator.
- Use 6–8 workers, preferably Terra or Sol, only when the requested feature has that many independent workstreams. Otherwise use the number that fits the work.
- Keep integration and final review with Astra. Workers should not change files outside their assigned ownership or create additional agents.

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

## Human verification handoff

Use this format when a runtime or behavioral check is appropriate:

> **Human verification needed:** Please run/perform: `<specific user action>`.
> **Expected result:** `<observable outcome>`.
> **Report back:** `<specific output or pass/fail detail Astra needs>`.

Do not label the behavior verified before receiving the human's result.

