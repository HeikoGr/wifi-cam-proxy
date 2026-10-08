# Agent instructions

Shared instructions for AI coding agents (GitHub Copilot, Claude Code, others).
`CLAUDE.md` and `.github/copilot-instructions.md` point here.

## Project

ESP32 firmware (PlatformIO, Arduino framework) that connects to cheap Wi-Fi cameras and serves
their image over Ethernet (ZB-GW03, WT32-ETH01) or on a display (CYD). See [README.md](README.md)
and [firmware/README.md](firmware/README.md).

- `firmware/` – firmware sources (`src/`, `include/`), `platformio.ini`
- `tools/host-tests/` – host tests for the camera code (`tools/host-tests/run.sh`)
- `tools/ui-preview/`, `tools/cyd-preview/` – web UI and CYD screenshots without hardware
- `docs/` – project documentation and research notes

## Build and test

Use the Python environment from `setup-build-env.sh` (`.venv`).

```bash
cd firmware && pio run -e zb-gw03 -e wt32-eth01 -e cyd   # all boards must build
tools/host-tests/run.sh                                  # host tests
```

Changes to camera protocols, the frame store or the sniffer need a passing host test run.
Never commit `firmware/include/secrets.h` (credentials, git-ignored).

## Language

Code, comments, commit messages, documentation and UI texts are written in English.

## Commits

Make small commits: **one logical change per commit**. A commit must build on its own.

- Stage selectively (`git add -p`); never `git add -A` blindly. Unrelated changes go into separate
  commits, also within one file.
- Keep refactoring, behaviour changes, docs and CI changes in separate commits.
- Documentation that describes a change may go into the same commit as that change.
- Subject line in English, imperative mood, no trailing period, at most 72 characters.
- Start with the area when it helps, as in the existing history: `CYD:`, `JHCMD:`, `MAX-VIEW:`,
  `Stream:`, `Web UI:`, `Sniffer:`, `Docs:`, `CI:`, `VS Code:`. No other prefix scheme.
- Add a body (wrapped at 72) only to explain *why*, e.g. measurements or protocol findings.
- Commits made with an AI tool end with a `Co-Authored-By:` line naming it, e.g.
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Do not push, tag, amend or rewrite published history unless asked.

Good: `JHCMD: heartbeat every 3 s like the app`
Bad: `fixes and improvements`, `WIP`, a commit touching firmware, docs and CI at once.

## Changelog

[CHANGELOG.md](CHANGELOG.md) follows [Keep a Changelog](https://keepachangelog.com/).
For every user-visible change (new feature, behaviour change, fix, removal) add one line under
`## [Unreleased]` in the same commit, in the matching group: `Added`, `Changed`, `Fixed`,
`Removed`. Pure refactoring, tests, docs and tooling need no entry unless they matter to users.
Releases are CI builds (`build-<run>-<sha>`); when one is published, move the `Unreleased`
entries under a heading with that tag name.

## Code style

- Match the surrounding code; keep changes minimal and on topic.
- Comments only for what the code cannot show (protocol quirks, hardware limits); one short line.
- The ESP32 has little RAM and no PSRAM: watch heap use, avoid per-frame allocations.
- Keep documentation in sync when behaviour, endpoints or settings change.
