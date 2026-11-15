# NexusAI as a DeepSeek Harness model

[DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness) (DSH, `dsh`) is DeepSeek's agent
harness: an agent loop with file, shell and search tools, sandboxed permission presets and
persistent sessions. NexusAI can be its model. DSH talks to NexusAI's OpenAI-compatible inference
API on **:8081**; it never uses the control API on :8082.

The Nexus desktop apps (NexusNix, NexusWindows) and NexusOnline (through NexusBackend) drive DSH as an
agent with NexusAI as the model. This page covers the NexusAI side and running DSH by hand.

## Set up

```bash
scripts/nexusai-dsh.py --base-url http://192.168.1.50:8081/v1 --smoke
```

The script:

1. Reads `GET /v1/models` from NexusAI, the model currently loaded and its context size (`meta.n_ctx`).
2. Writes `~/.dsh/nexusai.patch.yml` (mode 0600, folder 0700): a DSH patch overlay that declares a
   `nexusai` provider route (`api: openai-completions`) for those models and makes it the default
   model of new DSH agents.
3. With `--smoke`, runs one tool-free `dsh --profile headless` task through the overlay and requires
   the reply `OK`.

Use `--print` to see the overlay without writing it, `--model` to pick a model, `--out` for another
path. Cleartext HTTP is accepted only for loopback, private, link-local and Tailscale addresses (the
same rule as every Nexus client). After you load a different model in NexusAI, run the script
again: DSH's route lists exact model ids.

## Run

```bash
export NEXUSAI_API_KEY=nexusai-no-auth
dsh --profile headless --patch ~/.dsh/nexusai.patch.yml "summarise this repository"
dsh --profile headless --patch ~/.dsh/nexusai.patch.yml --json "…"       # NDJSON run events
dsh web --patch ~/.dsh/nexusai.patch.yml                                  # DSH's web UI
```

- `NEXUSAI_API_KEY` is a placeholder. :8081 has no authentication, but DSH refuses a hand-declared
  route without a credential reference, and the overlay names this variable instead of storing a
  value.
- The overlay is passed with `--patch` on each run, so your own DSH profiles stay untouched.
- Headless runs use DSH's permission presets: `--permission-mode read-only | workspace-write |
  danger-full-access` and `--approval-policy never` (headless cannot ask).

## Installing DSH

The official package is `@deepseek-ai/dsh` on npm (repository `deepseek-ai/deepseek-harness`). It needs
Node.js 22.19 or newer. On NixOS, DSH's native module loader fails with NixOS's own Node build
(`node-addon-require-builtin unsupported: Unsupported/no-getter`), so run it with the official
nodejs.org Node 22 binary (needs `programs.nix-ld`):

```bash
mkdir -p ~/.local/share/dsh && cd ~/.local/share/dsh
npm install @deepseek-ai/dsh                       # pin the version you reviewed
# unpack node-v22.x-linux-x64.tar.xz (checked against SHASUMS256.txt) into ~/.local/share/dsh/node
cat > ~/.local/bin/dsh <<'EOF'
#!/bin/sh
PATH="$HOME/.local/share/dsh/node/bin:$PATH" exec "$HOME/.local/share/dsh/node_modules/.bin/dsh" "$@"
EOF
chmod 755 ~/.local/bin/dsh
```

## What a run looks like

`--json` prints one event per line. These were observed from DSH 0.2.2-rc.1 against NexusAI:

```json
{"type":"session","sessionId":"session-…","cwd":"/path/to/workspace"}
{"type":"status","phase":"step_start","turn":1,"step":1}
{"type":"thinking","text":"…"}
{"type":"tool_call","callId":"…","tool":"read","input":{"file_path":"/path/note.txt"}}
{"type":"tool_result","callId":"…","status":"completed","result":"…"}
{"type":"text","text":"The file note.txt contains …"}
{"type":"status","phase":"step_end","turn":1,"step":2,"usage":{"inputTokens":113,"outputTokens":34,"totalTokens":5935}}
{"type":"final","sessionId":"session-…","reason":{"kind":"completed"},"text":"…"}
```

A failed run ends with `"reason":{"kind":"error","error":{"message":…,"code":…}}` and exit status 1.
`--session-id <id>` continues a session.

## Verified

15 November 2026, DSH 0.2.2-rc.1, official Node 22.23.5 on NixOS, NexusAI on a PS5 serving Ternary
Bonsai 2 27B (Q2_0, 65,536-token context):

- `scripts/nexusai-dsh.py --smoke` wrote the overlay and DSH replied `OK` (about 75 s per turn; DSH's
  system prompt is about 5,700 tokens).
- A tool task: DSH read a file with its `read` tool and answered from the contents.
- `dsh --dump-config` composed the overlay with no loader diagnostics.

**Not verified:** DSH's web UI, long multi-step agent tasks, and edits or commands made by the model
on NexusAI. A 27B Q2 model is slow on the PS5 and weaker at tool use than a hosted model.
