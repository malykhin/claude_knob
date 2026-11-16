# Security policy

## Supported versions

Only the latest release gets security fixes.

## Reporting a vulnerability

Please don't open a public issue. Report it privately from the repository's **Security** tab with **Report a vulnerability**. Only the maintainer can see the report.

Say what the problem is, how to reproduce it and what an attacker could do with it.

## Scope

The parts most worth a look:

- `knobd hook`, which Claude Code runs on every hook event, and the unix socket it talks to (`~/.claude-knob/knobd.sock`).
- The installer (`install.sh`) and the Claude Code hooks it adds to `settings.json`.
- The daemon's handling of messages from the knob over USB serial.
