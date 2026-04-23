# Design: Multi-Machine Frame Sync

## Problem

A single PC can drive N displays via `CopySubresourceRegion` blits from one CEF OSR texture. Beyond what one GPU can handle — or when physical cabling makes a single-machine topology impossible — you need multiple PCs, each rendering a slice of the total surface and presenting in visual lockstep.

## Why Not Just NTP?

NTP is accurate to ~10–50ms on a LAN. A 60fps frame is ~16.7ms. NTP alone gives you content that drifts in and out of sync visibly. The solution is a dedicated application-layer clock broadcast that can be calibrated to sub-frame accuracy because it only needs to agree within ~1ms, and it runs entirely on your LAN without depending on internet time servers.

## Layer 1: Content Sync

The key insight is that `requestAnimationFrame` does not need to fire at the same moment across machines — it just needs to advance the same logical time. A shared monotonic clock means every machine computes identical content state at every frame, regardless of minor scheduling differences.

**Protocol:** UDP is preferred over WebSocket for the conductor → client broadcast because it has lower latency, no connection overhead, and packet loss is acceptable (client self-corrects on the next packet). Multicast is ideal on a managed switch; unicast fallback works on any LAN.

**Jitter smoothing:** Network jitter of 1–3ms is typical on a LAN. A simple exponential moving average of the offset between received `t` and local `performance.now()` converges in ~10 frames and keeps drift under 0.5ms thereafter.

**JS injection point:** chromeyumm already injects `window.__chromeyumm` via the CEF `OnContextCreated` path. `window.__sharedClock` follows the same pattern — injected before page JS runs, so sketches can depend on it at load time.

## Layer 2: Frame Hold

Even with identical content state, each machine's GPU presents at an independent phase — monitors start their scanout at arbitrary offsets relative to each other. For content with hard horizontal edges crossing display seams this is visible as a seam "tear" that crawls slowly (the period is the beat frequency between the two vsync clocks, typically many seconds).

**Mechanism:** After `OnAcceleratedPaint` delivers a new DXGI texture, instead of immediately calling `CopySubresourceRegion`, the render thread parks on a condition variable. A background UDP listener signals it when the conductor's sync pulse arrives. The conductor sends the pulse after its own `Present` call, so all clients present one network-RTT behind the conductor — acceptable and consistent.

**Timeout:** A missed pulse must not stall the display indefinitely. A configurable timeout (default: 2× frame interval) releases the hold and presents immediately. This produces at most one torn frame per dropped packet, which is indistinguishable from normal content at installation viewing distances.

**Thread model:** The existing render thread in `cef-wrapper.cpp` owns `CopySubresourceRegion` and `Present`. The UDP listener runs on its own thread and signals via a `std::condition_variable`. No new locking complexity beyond what the existing Spout path already uses.

## Interactive Content and State Sync

### Why the shared clock is not enough

A shared clock solves the "what time is it" problem but not the "what happened" problem. Interactive content diverges the moment a user input occurs on one machine that the others don't know about. The simulation on each machine then takes a different path forward, producing visibly different content even though the clocks agree.

### Why not replicate raw input events?

Broadcasting raw input events (mouse position, keystrokes) to all machines is conceptually simple but has a sequencing problem: the event needs to arrive and be applied by all machines before any of them advance to the next frame. On a LAN with ~1ms RTT, this means either:
- Adding deliberate input lag (hold each frame until all clients ACK the event), or
- Accepting that fast-moving input will produce brief divergence while the event propagates

For a multiplayer game, this is the core netcode problem and entire careers are spent on it. For an installation, it is almost always the wrong abstraction — installations typically have coarser, lower-frequency input (someone touches a screen, a sensor fires, an OSC message arrives) where a one-frame lag is invisible.

### The shared state model

Rather than replicating events, the conductor maintains a small authoritative state object and broadcasts it alongside the clock packet. The distinction matters:

- **Events** are ephemeral and order-dependent (`mouse moved to 312, 490`)
- **State** is the current truth regardless of how it was reached (`attractor is at 312, 490`)

Content reads state, not events. The conductor applies input locally and immediately; clients are always one broadcast-interval behind but consistently so. For 60fps broadcast that's ≤16ms — imperceptible for installation interaction.

The state object should be **small and flat**. If it grows large enough that broadcasting it at 60fps stresses the LAN, that is a signal to move to Model C (conductor-authoritative full state push) where the conductor serializes its simulation output each frame and clients are pure renderers.

### The lockstep alternative and why to avoid it

Lockstep networking (used in RTS games) makes all machines advance in unison: no machine steps forward until all machines have submitted their inputs for the current logical tick. This guarantees identical state with zero reconciliation. The cost is that the slowest machine determines the frame rate for everyone. In a game that's tolerable; in an installation with an audience watching a wall of screens, one machine hiccupping drops the whole rig to 10fps. Not suitable here.

## Why Not SMPTE Timecode or PTP?

SMPTE timecode requires dedicated AJA/Blackmagic hardware. PTP (IEEE 1588) requires switch support and a PTP daemon — achievable but heavy infrastructure for an installation context. The UDP application clock described here is good enough for browser-based generative content and requires only a LAN.

## Comparison to nDisplay / Notch

Those systems solve the same two layers but go further:

- **Hardware genlock**: NVIDIA Quadro Sync cards share an electrical sync signal across GPUs, making vsync truly phase-locked. This eliminates L2 entirely in hardware.
- **Cluster management**: They handle topology discovery, failover, and frame-perfect video playback. Overkill for generative content.

Our approach gives ~95% of the perceptual result at ~5% of the infrastructure cost, which is the right tradeoff for the chromeyumm use case.

## Related

- [../product-specs/multi-machine-sync.md](../product-specs/multi-machine-sync.md) — Feature spec and phased plan
- [single-master-gpu-blit.md](single-master-gpu-blit.md) — Architecture this extends
