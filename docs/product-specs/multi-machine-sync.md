# Feature: Multi-Machine Frame Sync

## User Story

As a live visual artist, I want to run chromeyumm across multiple PCs and have their content stay in sync, so I can drive a display surface larger than a single machine can handle.

## Overview

Two independent sync layers, each shippable on its own:

| Layer | What it solves | Risk |
|---|---|---|
| **L1 — Content sync** | All machines render the same logical frame (same time, same RNG seed) | Low — purely additive |
| **L2 — Frame-hold** | Defer `Present` until a sync pulse so scanout phase-aligns | Medium — touches `OnAcceleratedPaint` in C++ |

L1 alone is sufficient for ambient/generative visuals with soft edges. L2 matters for hard-edged content crossing display seams.

## Architecture

### L1 — Shared Clock (Bun UDP conductor)

A single "conductor" machine runs a small Bun UDP server that broadcasts a monotonic packet every frame (or on a fixed interval). Each client instance:

1. Receives the packet and computes a local offset (`sharedTime - performance.now()`)
2. Maintains a simple rolling average to smooth jitter
3. Exposes `window.__sharedClock.now()` in the CEF page via the existing JS injection path

Sketches opt in by replacing `performance.now()` / p5's `millis()` / Three.js clock with `window.__sharedClock.now()`. The conductor also broadcasts a `frameCount` integer so seeded RNG stays locked.

Each machine's `display-config.json` already describes which region of the virtual canvas it owns — no new config needed for the content-slicing concern.

### L2 — Frame Hold (C++ `OnAcceleratedPaint`)

Add a hold flag to the render path in `cef-wrapper.cpp`:

```
OnAcceleratedPaint fires
  → if syncMode == HOLD: store texture handle, arm timeout, wait for UDP pulse
  → on pulse (or timeout): CopySubresourceRegion → Present
  → if syncMode == FREE (default): existing path, no change
```

A configurable timeout (default ~2 frames) ensures a dropped packet never freezes the display. Free-run is the default; hold mode is opt-in via `display-config.json`.

## Configuration

```json
{
  "multiMachineSync": {
    "role": "conductor",        // "conductor" | "client"
    "conductorHost": "192.168.1.10",
    "port": 45678,
    "frameLock": false          // true enables L2 frame-hold (C++ path)
  }
}
```

Conductor also runs normal chromeyumm rendering — it is not a dedicated server process.

## Phased Implementation

### Phase 1 — L1 content sync (low risk)
- [ ] Bun UDP conductor: broadcast `{ t: number, frame: number }` packets
- [ ] Bun UDP client: receive, compute offset, smooth jitter
- [ ] JS injection: expose `window.__sharedClock` via existing CEF injection path
- [ ] `display-config.json` schema: `multiMachineSync` block
- [ ] Demo sketch: p5.js example that uses `window.__sharedClock`

### Phase 2 — L2 frame-hold (medium risk)
- [ ] Add `syncMode` flag and UDP pulse listener to `cef-wrapper.cpp`
- [ ] Gate `CopySubresourceRegion` on pulse arrival
- [ ] Timeout fallback (free-run after N ms without pulse)
- [ ] `frameLock: true` wires this path from config

### Phase 3 — Shared state for interactive content (low-medium risk)
- [ ] Extend UDP broadcast packet to carry a `state` payload alongside clock
- [ ] Conductor: collect `__sharedState.set()` calls between frames, include in next packet
- [ ] Client: apply received state before next `rAF` tick
- [ ] JS injection: expose `window.__sharedState` API (`get`, `set`, `onChange`)
- [ ] Demo sketch: interactive p5.js example (e.g. shared attractor point driven by mouse on conductor)

## Content Authoring

### The determinism requirement

For sync to work, all machines must produce identical content state given the same inputs. Time-driven content satisfies this automatically once you replace `performance.now()` with `window.__sharedClock.now()`. Interactive content does not — user input is local to one machine, so simulations diverge the moment a mouse moves or a button is pressed.

### Three content models

**Model A — Pure time-driven (no interaction)**
Content is a pure function of `(time, seed, viewportSlice)`. This is the simplest case and the only one L1 alone handles completely. RNG must be seeded from `window.__sharedClock.frame` rather than from a random init value, otherwise each machine starts with a different seed.

**Model B — Shared state (recommended for interactive content)**
The harness exposes `window.__sharedState` — a small plain object that the conductor owns and broadcasts each frame alongside the clock packet. User interaction updates state on the conductor; clients receive it and treat it as read-only. All machines then render identically because they're all reading the same state.

Content authors define what goes in `__sharedState` — it should be the minimal set of values that drives the simulation (e.g. current attractor position, active palette index, physics seed override). Raw input events do not belong there; derived state does.

```js
// conductor machine — content writes state
window.__sharedState.set({ attractorX: mouseX, attractorY: mouseY })

// all machines (including conductor) — content reads state
const { attractorX, attractorY } = window.__sharedState.get()
```

The conductor broadcasts state after each local frame update. Clients apply the latest received state before their next `rAF` tick. One-frame-of-latency lag is imperceptible for installation contexts.

**Model C — Conductor-authoritative (full state push)**
The conductor runs the entire simulation and serializes its full output state each frame. Clients are pure renderers. This eliminates divergence with no authoring discipline required, but demands that state be compact enough to broadcast at 60fps (~16ms budget). Works well for physics sims where state is a flat array of positions/velocities. Too expensive for complex scene graphs.

### Choosing a model

| Content type | Recommended model |
|---|---|
| Generative / time-driven (no input) | A |
| Interactive installation (touch, sensors, OSC) | B |
| Physics sim, particle system with interaction | B or C depending on state size |
| Game-like with tight input feedback loop | Out of scope — latency constraints are incompatible with multi-machine broadcast |

### Harness additions required for Model B

- `window.__sharedState.get()` — returns current state object (read-only on clients)
- `window.__sharedState.set(patch)` — merges patch into state; no-op on clients, broadcast on conductor
- `window.__sharedState.onChange(fn)` — optional callback when state arrives from conductor

This is a Phase 3 addition, after L1 and L2 are validated.

## Out of Scope

- Hardware genlock (requires NVIDIA Quadro Sync or AMD FirePro sync cards — not consumer hardware)
- PTP/IEEE 1588 sub-millisecond network clock (overkill for browser-based content)
- Automatic topology discovery across machines (manual config is fine)
- Per-machine content differentiation beyond viewport slicing

## Edge Cases

- **Network jitter**: rolling-average offset smoothing in the client; a 1–2ms jitter window is imperceptible for generative content
- **Client joins late**: conductor broadcasts unconditionally; client self-corrects within a few frames once packets arrive
- **Packet loss (L2)**: timeout fallback prevents display freeze; one missed pulse causes at most one torn frame
- **Firewall / multicast not available**: fall back to unicast UDP; conductor iterates known client IPs from config

## Related

- [multi-window-d3d-output.md](multi-window-d3d-output.md) — Single-machine multi-display, the foundation this builds on
- [display-config.md](display-config.md) — Config format this extends
- [../design-docs/multi-machine-sync.md](../design-docs/multi-machine-sync.md) — Technical rationale and tradeoffs
