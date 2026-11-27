# Codex Workflow for Chiffon

These instructions define the default multi-agent workflow for feature work in this repository when Codex is used.

## Orchestrator

- The orchestrator must use an Astra-class model (prefer Astra) and own the feature plan, architecture decisions, task assignment, integration, review, and final report.
- For a multi-part feature, use 6–8 worker subagents when the work can be divided into genuinely independent pieces. Prefer Terra or Sol workers. If fewer independent tasks exist, use fewer workers rather than creating artificial parallelism.
- Workers implement bounded tasks and report files changed, design choices, risks, and build status. They do not assign further agents unless the orchestrator explicitly asks.
- The orchestrator checks for overlapping edits, integrates the work, reviews the complete diff, resolves inconsistencies, and reports any remaining gaps.

## Required sequence

1. Read the root `AGENTS.md`, inspect the working tree, and identify existing user changes before editing.
2. Convert the requested outcome into a short plan with affected components, dependencies, both-architecture considerations, acceptance criteria, and a verification plan.
3. Split only independent work among 6–8 Terra/Sol workers where practical. Give each worker a narrow scope, expected output, and explicit file ownership to reduce conflicts.
4. Have workers report back before integrating. Review changes against the plan and Chiffon's hybrid-kernel architecture. Keep scheduling, PMM, VMM, IPC, capabilities, and established core services in their existing ownership boundaries; preserve user-space peripheral servers and capability/IPC boundaries.
5. Compile verification is restricted to the single command below, run on the user's WSL instance. Workers and Astra must not run this command from Windows, a container, a remote shell, or another WSL instance. Do not run other compile, test, lint, formatting, emulator, boot, or runtime commands as verification. Do not claim any check that was not actually run.
6. Runtime and behavioral verification belong to the human. When such verification is relevant, stop and give the human clear, concise steps to run and the expected observable result. Astra must not perform those direct checks.
7. Review the final diff for unintended changes, error paths, architecture consistency, and documentation. Summarize files changed, build result, and any human verification still needed.

## Worker verification boundary

Workers may inspect source and reason about changes. Their only allowed verification command is this exact command, executed on the user's WSL instance:

```sh
make ENV=milkos BUSYBOX=$HOME/busybox/busybox
```

Do not modify the command, add flags/targets, substitute another environment or BusyBox path, or run it in a different environment. Astra may run it only through the user's WSL instance as well. If a worker or Astra cannot access that WSL instance, report that the build was not run and ask the human to run the exact command; do not try a local, Windows-host, container, emulator, or alternate build as a substitute.

No worker or Astra may directly verify behavior by booting, running, testing, or interacting with Chiffon. Only the human may perform those checks. When a behavior check is needed, Astra must provide precise steps and expected results, then stop and wait for the human's report before claiming the behavior is verified or moving the feature to verified/completed status. If no runtime check is appropriate, Astra must say that human runtime verification was not applicable rather than implying it happened.

The compile command is a build check only; it does not count as runtime or behavioral verification. Astra owns final integration and source review, while the human owns all direct behavior checks.

## Subagent task template

For each worker, Astra should state:

- **Task:** one independent implementation or investigation outcome.
- **Owned paths:** files the worker may edit; do not edit outside them without reporting first.
- **Dependencies:** required decisions or interfaces from other tasks.
- **Acceptance criteria:** observable code/documentation conditions.
- **Constraints:** preserve existing user changes and the hybrid architecture; no direct runtime verification; the exact WSL compile command above is the only allowed verification.
- **Return report:** summary, paths changed, decisions/assumptions, risks, and whether the allowed build was run on WSL (include outcome or state not run).

