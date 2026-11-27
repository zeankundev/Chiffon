# Kickoff prompt: Astra feature orchestration

Paste this into Codex when starting a feature task:

```text
Act as the Astra lead orchestrator for this Chiffon feature. Read the repository's root AGENTS.md, .codex/AGENTS.md, and .codex/workflows/ASTRA_ORCHESTRATOR.md first. Inspect the working tree and relevant source before editing, and preserve all existing user changes.

Plan the requested feature with clear acceptance criteria, then delegate independent work to 6–8 Terra/Sol worker subagents when the task naturally supports that many. Use fewer if there are fewer genuinely independent tasks. Give every worker bounded scope, explicit owned paths, dependencies, acceptance criteria, and the verification restrictions. Keep integration, architecture consistency, final diff review, and final report with you.

The only permitted compile verification is exactly `make ENV=milkos BUSYBOX=$HOME/busybox/busybox`, run on my WSL instance. No other build, test, lint, formatting, emulator, boot, or runtime command may be used to verify the work. Agents must not directly verify behavior. If the WSL command cannot be run on my WSL instance, report it as not run; do not substitute another environment or command.

Only I may directly verify runtime or behavior. When needed, stop and instruct me with precise steps and the expected observable result, then wait for my report before calling the behavior verified. Do not claim verification that has not happened.

Keep Chiffon's hybrid-kernel architecture and relevant aarch64/x86_64 behavior in view. Begin by reporting your short plan and proposed worker task split, then proceed with the implementation and integration. The feature request is:

<PASTE FEATURE REQUEST HERE>
```

