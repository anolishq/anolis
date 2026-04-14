# Anolis Operator UI

A **minimal dev/operator tool** for validating the Anolis HTTP API.

> **Note**: This is NOT a production UI. It is a dev-only sanity tool proving the API is human-operable.

## Quick Start

### 1. Start the Anolis Runtime

```bash
cd /path/to/anolis
./build/dev-release/core/anolis-runtime --config /path/to/anolis-runtime.yaml
```

### 2. Open the UI

#### Option A: Helper script (recommended)

_Simplest method - uses Python HTTP server._

```bash
# Windows
cd tools/operator-ui
.\serve.ps1

# Linux/macOS
cd tools/operator-ui
./serve.sh

# Then open http://localhost:3000
```

#### Option B: Direct file open

_May have CORS issues with some browsers._

```bash
# macOS
open tools/operator-ui/index.html

# Linux
xdg-open tools/operator-ui/index.html

# Windows
start tools/operator-ui/index.html
```

#### Option C: Manual HTTP server

_Same as Option A but manual._

```bash
# Python 3
python -m http.server 3000 -d tools/operator-ui

# Then open http://localhost:3000
```

#### Option D: VS Code Live Server

1. Install the "Live Server" extension
2. Right-click `index.html` → "Open with Live Server"

## Features

### Device List

- Shows all devices from `GET /v0/devices`
- Grouped by provider
- Click to select and view details

### State View

- Live-updating state via `GET /v0/state/{provider}/{device}`
- Polls every 500ms (configurable in code)
- Shows signal values with type, quality badge, and age
- Pause/Resume polling button

### Function Invocation

- Auto-generated forms from `GET /v0/devices/{provider}/{device}/capabilities`
- Type-aware input fields (double, int64, uint64, bool, string, bytes)
- Executes via `POST /v0/call`
- Shows success/error feedback

### Automation Panel (when enabled)

**Automation Status:**

- Current mode display (MANUAL, AUTO, IDLE, FAULT)
- Active policy name
- Loaded behavior tree file
- Last tick timestamp
- Mode selector dropdown to switch modes

**Parameters Panel:**

- Live parameter list with current values
- Type-aware update controls (int64, double, bool, string)
- Inline validation and feedback
- Real-time updates via SSE events

**Behavior Tree Visualization:**

- Text outline view of loaded BT structure
- Shows node hierarchy with indentation
- Fetched from `/v0/automation/tree` endpoint

**Event Trace:**

- Real-time event stream (mode changes, parameter updates, provider events)
- Ring buffer (last 100 events)
- Color-coded by event type
- Auto-scrolling to most recent

### Runtime Status

- Header shows runtime connection status
- Green = connected, Red = unavailable
- Real-time updates via Server-Sent Events (SSE)

## Contract Normalization

Operator UI uses `js/contracts.js` as its runtime HTTP shape adapter.

Examples:

1. Normalizes state signal timestamps (`timestamp_epoch_ms` -> `timestamp_ms`).
2. Normalizes capabilities/parameters collection extraction.
3. Parses SSE JSON payloads consistently across event types.

Fixture contract tests for this module run against canonical runtime HTTP examples
from `tests/contracts/runtime-http/examples`.

## Configuration

Edit `js/config.js` to change:

```javascript
const API_BASE = "http://localhost:8080"; // Runtime HTTP address
const POLL_INTERVAL_MS = 500; // State polling interval
```

## Design Constraints

This UI follows strict constraints by design:

| Constraint             | Reason                        |
| ---------------------- | ----------------------------- |
| No framework           | Keep it simple, no build step |
| No runtime npm/node build step | Static assets served directly |
| Capability-driven only | No device-type assumptions    |
| No charts/graphs       | Grafana territory             |
| No auth                | Future Work                   |
| SSE for events         | Real-time updates             |

The UI is a **mirror** of the HTTP API, not a new abstraction. It must not introduce new semantics.

## Files

```sh
tools/operator-ui/
├── index.html
├── package.json
├── style.css
├── js/
│   ├── app.js
│   ├── api.js
│   ├── contracts.js
│   ├── automation.js
│   ├── device-overview.js
│   ├── device-detail.js
│   ├── sse.js
│   ├── telemetry.js
│   ├── ui.js
│   └── config.js
├── tests/
│   └── contracts.test.mjs
└── README.md
```

## Local Contract Test

```bash
node --test tools/operator-ui/tests/contracts.test.mjs
```

If `node` is unavailable locally, `tools/verify-local.sh` skips this step.

Contract dependencies:

1. Runtime HTTP OpenAPI: `schemas/http/runtime-http.openapi.v0.yaml`
2. Runtime HTTP fixture source: `tests/contracts/runtime-http/examples/`

## Troubleshooting

### "Failed to connect to runtime"

- Ensure `anolis-runtime` is running
- Check it's bound to `127.0.0.1:8080`
- Check browser console for CORS errors

### CORS Errors

- Use Option B (python HTTP server) or Option C (Live Server)
- Or disable CORS in your browser for local dev (not recommended)

### State not updating

- Check the "Polling" status in the UI
- Click "Resume" if paused
- Check browser console for errors

## API Endpoints Used

| Endpoint                               | Purpose                      |
| -------------------------------------- | ---------------------------- |
| `GET /v0/runtime/status`               | Connection health check      |
| `GET /v0/devices`                      | Device discovery             |
| `GET /v0/devices/{p}/{d}/capabilities` | Capability introspection     |
| `GET /v0/state/{p}/{d}`                | Live state polling           |
| `POST /v0/call`                        | Function invocation          |
| `GET /v0/mode`                         | Get current automation mode  |
| `POST /v0/mode`                        | Set automation mode          |
| `GET /v0/parameters`                   | Get runtime parameters       |
| `POST /v0/parameters`                  | Update runtime parameter     |
| `GET /v0/automation/tree`              | Get behavior tree XML        |
| `GET /v0/events`                       | SSE event stream (real-time) |
