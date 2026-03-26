# Feature: Web Components

## User Story
As a developer building visuals for Chromeyumm, I want drop-in web components for debugging, Spout input, and layout awareness, so I can add Chromeyumm tooling to any web page without a build step or framework dependency.

## Components

### `<debug-panel>` — Debug Panel
**File:** `src/components/debug-panel.js`
**Element:** `<debug-panel>`
**Dependency:** `stats.js` (npm)

Comprehensive debug overlay with:
- **Keys section** — hotkey reference with live toggle states (alwaysOnTop, interactiveMode) read from `window.__ebState`
- **Spout section** — live sender/receiver status, FPS counter, dimensions. Auto-shown when Spout URL params are present.
- **Perf section** — stats.js FPS/MS/MB graphs. Hidden until first `stats.begin()` call.
- **View sections** — extensible via `panel.update({ render, canvas, mouse })`

**Injected global:** `window.__ebPanelToggle()` — called by Ctrl+D shortcut.

**API:**
```javascript
import "../../components/debug-panel.js";
const panel = document.querySelector("debug-panel");

panel.onOpen = () => refresh();     // called immediately on open
panel.update({                      // push view-specific data
  render: "12 draw calls · 45,230 tris",
  canvas: "1920×1080 px",
  mouse:  "(640, 360)",
});

// Wrap render loop for perf graphs:
panel.stats.begin();
renderer.render(scene, camera);
panel.stats.end();
```

---

### `<slot-overlay>` — Slot Boundary Debug Overlay
**File:** `src/components/slot-overlay.js`
**Element:** `<slot-overlay>`
**Dependencies:** None

Visual debug overlay showing:
- Coordinate grid (100px intervals with labels)
- Per-slot colored boundary boxes with dimensions and coordinates
- Simulation mode indicator (`[sim]` label)

**Injected global:** `window.__ebDebugToggle()` — called by Ctrl+D shortcut.

**URL params consumed:** `totalWidth`, `totalHeight`, `slots` (JSON array)

---

### `<spout-receiver>` — Spout Input Receiver
**File:** `src/components/spout-receiver.js`
**Element:** `<spout-receiver>`
**Dependencies:** None

Self-contained Spout frame receiver with:
- Two input modes: shared memory (zero-copy) or persistent buffer (sandbox-safe)
- WebGL rendering with BGRA→RGBA swizzle shader
- Checker overlay that fades in when feed disconnects (>1s stall)

**Custom events:**
- `spout-connect` — first frame received; `detail: { width, height }`
- `spout-frame` — every new frame; `detail: { pixels, width, height }` (BGRA)
- `spout-disconnect` — feed stalled >1s

**Usage modes:**
```html
<!-- Direct render (default) -->
<spout-receiver style="width:100%;height:100%"></spout-receiver>

<!-- Events-only (pipe into Three.js, etc.) -->
<spout-receiver render="false"></spout-receiver>
```

**Three.js integration:**
```javascript
el.addEventListener('spout-frame', ({ detail: { pixels, width, height } }) => {
  tex.image = { data: pixels, width, height };
  tex.needsUpdate = true;
  // pixels are BGRA — swizzle in your shader
});
```

---

### `<spout-video>` — Spout to Video Element
**File:** `src/components/spout-video.js`
**Element:** `<spout-video>`
**Dependencies:** `spout-receiver.js` (same directory)

Extends `<spout-receiver>` — wraps Spout frames in a native `<video>` element via `canvas.captureStream()`. The GL canvas renders off-screen; the video element displays the frames.

**Exposed properties:**
- `el.video` — the inner `HTMLVideoElement` (useful for PiP, recording, MediaStream piping)

**External CSS:** `spout-video::part(video) { ... }`

---

### `parseLayoutParams()` — Layout Parameter Parser
**File:** `src/components/layout-params.js`
**Type:** Utility function (not a web component)

Parses standard Chromeyumm URL parameters injected by the app:

| Param | Type | Default | Description |
|---|---|---|---|
| `slot` | number | 0 | Zero-based window index |
| `totalSlots` | number | 1 | Total display slots |
| `totalWidth` | number | 1920 | Virtual canvas width |
| `totalHeight` | number | 1080 | Virtual canvas height |
| `sourceX` | number | 0 | Left edge of this slot's region |
| `sourceY` | number | 0 | Top edge of this slot's region |
| `sourceWidth` | number | totalWidth | Width of this slot's region |
| `sourceHeight` | number | totalHeight | Height of this slot's region |
| `simulated` | boolean | false | Running in simulation mode |
| `spoutReceiverId` | number | 0 | Spout input receiver ID |

```javascript
import { parseLayoutParams } from "../../components/layout-params.js";
const { totalWidth, totalHeight, slot, simulated } = parseLayoutParams();
```

## Design Philosophy
- **Framework-agnostic** — pure vanilla JS, Custom Elements API
- **Zero build tools** — drop `.js` files into any project
- **Shadow DOM** — styles are encapsulated, won't conflict with page CSS
- **Event-driven** — components communicate via standard CustomEvents (bubbles + composed)
- **Progressive enhancement** — components degrade gracefully when globals aren't present (e.g., no Spout input → shows info message)

## How They Work Together

```
Chromeyumm app (src/app/index.ts)
├─ Injects URL params: ?totalWidth=N&totalHeight=M&slots=[...]&spoutReceiverId=X
├─ Registers Ctrl+D → window.__ebPanelToggle() + window.__ebDebugToggle()
└─ Loads view (e.g., threejs) with:
   ├─ <debug-panel> — shows perf + spout status
   ├─ <slot-overlay> — shows slot grid + boundaries
   └─ <spout-receiver render="false"> — polls frames for Three.js texture
```

## Injected Globals

| Global | Injected By | Purpose |
|---|---|---|
| `window.__ebPanelToggle()` | `<debug-panel>` connectedCallback | Toggle debug panel visibility |
| `window.__ebDebugToggle()` | `<slot-overlay>` connectedCallback | Toggle slot overlay visibility |
| `window.__ebState` | CEF on dom-ready + Ctrl+F/M | `{ alwaysOnTop, interactiveMode }` |
| `window.__spoutFrameBuffer` | CEF OnContextCreated | ArrayBuffer for zero-copy frame polling |
| `window.__spoutGetFrame` | CEF OnContextCreated | Function(buf)→bool for sandbox mode |
| `window.__spoutDiag` | C++ runtime (optional) | Diagnostic string shown in debug panel |

## Out of Scope
- React/Vue/Svelte wrappers
- TypeScript declarations (components are plain JS)
- NPM package publishing
- Automated component testing

## Related
- [../FRONTEND.md](../FRONTEND.md) — Browser-side architecture
- [spout-input.md](spout-input.md) — Spout input feature spec
- [hotkeys.md](hotkeys.md) — Ctrl+D shortcut that triggers these components
