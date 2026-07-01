# Velox CLI

`velox-cli` is the first-party command-line client for VeloxDB. It speaks RESP2 over TCP and is
included in official VeloxDB container packages.

VeloxDB does not vendor the CLI as a submodule. The server remains independently buildable, while
the CLI can version and release on its own cadence.

## Build

For local CLI development, clone the CLI repository from the organization and build it with CMake:

```bash
gh repo clone buildyourownstuff/velox-cli
cd velox-cli
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Usage

Start VeloxDB:

```bash
./build/veloxdb --port 7379
```

Run direct commands:

```bash
velox-cli PING
velox-cli SET name dev
velox-cli GET name
velox-cli INCR counter
velox-cli INFO
```

Interactive mode starts when no command is provided:

```bash
velox-cli
127.0.0.1:7379> PING
PONG
```

Use raw mode for scripts:

```bash
velox-cli --raw GET name
```

## Defaults

- Host: `127.0.0.1`
- Port: `7379`
- Protocol: RESP2

Environment variables:

- `VELOXCLI_HOST`
- `VELOXCLI_PORT`

## Current Capabilities

- One-shot command execution.
- Interactive REPL.
- RESP2 command encoding.
- Incremental RESP2 reply parsing.
- Human-readable and raw output modes.
- Script-friendly exit codes.

## Planned

- Packaged binary releases.
- Shell completions.
- History file support.
- TLS and AUTH helpers.
- RESP3 support.
- JSON output mode.
