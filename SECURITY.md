# Security

## Reporting a vulnerability

Please report security issues privately through GitHub:
**Security → Report a vulnerability** on this repository. Don't open a public issue.

Include what you found, how to reproduce it, and which release or commit you tested.

## Scope

- The control API on :8082 (`native/nexus_control.cpp`) and its token handling.
- The patches applied to the upstream app.
- The provisioning, title and preflight tooling in this repository.

Issues in the upstream app that NexusAI doesn't change should also be reported to
[PS5LM Plus](https://github.com/abhi245y/PS5LM-Plus).

## Deployment basics

- Never expose :8081 or :8082 to the internet. Firewall :8082 to your gateway only.
- Keep the control token out of the console. Only its SHA-256 hash belongs in `nexus-control.token`.
- Use separate credentials for inference and model control.
