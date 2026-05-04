# Reliability

## Failure Modes and Recovery

### GPU Process Crash (Optimus)

**Symptom**: GPU subprocess (`chromeyumm Helper.exe`) crashes with exit code 143 at ~30 seconds.
**Cause**: Without `in-process-gpu: true`, the GPU thread runs in a subprocess where the `GpuPreference=2` registry entry doesn't apply. The GPU subprocess falls back to Intel iGPU, which lacks the D3D11 features CEF needs for OSR.
**Mitigation**: `in-process-gpu: true` is set in chromium flags. The GPU thread runs inside the main process where the registry preference applies.
**Detection**: Process exits within 30s of launch. Console output from CEF's GPU initialization.

### DXGI Shared Texture Handle Invalid

**Symptom**: `OpenSharedResource1` fails in `OnAcceleratedPaint` — no frames rendered.
**Cause**: D3D11 device not initialized through the `startD3DOutput` / `startSpoutSender` code path. The device must be created before `OnAcceleratedPaint` first fires.
**Mitigation**: Start D3D output or Spout sender before the master browser navigates to content. Current code does this correctly in `src/app/index.ts`.

### Monitor Topology Change

**Symptom**: Display windows appear on wrong monitors, overlap, or are invisible after monitor disconnect/reconnect/rearrange.
**Cause**: Win32 HWND positions are absolute screen coordinates. Monitor topology changes invalidate those coordinates.
**Recovery**: **Ctrl+Shift+M** destroys all NativeDisplayWindows + D3D output state, recreates them from config, and reloads content. This is manual recovery — there is no automated topology change detection.

### "Black Window" in OSR

**Symptom**: Master window shows only black. Appears to be rendering nothing.
**Diagnosis**: In OSR mode, the master HWND is always black — CEF doesn't paint to it. Content only exists in the `OnAcceleratedPaint` shared texture. To verify:
- Do a staging-texture pixel readback on the first frame
- `alpha=0` → texture is unrendered (CEF pipeline issue)
- `alpha=FF + black RGB` → CEF rendered but page content is black (CSS `background:#000` with failed JS load, or wrong URL)

### Spout Receiver Thread — Texture Data Lost

**Symptom**: Spout input frames appear corrupted or stale.
**Cause**: `ReceiveTexture()` was called in the outer loop after a `Map(DO_NOT_WAIT)` failure. This resets the internal sequence and loses the texture.
**Mitigation**: The retry loop for `Map(DO_NOT_WAIT)` must be an inner loop that does NOT call `ReceiveTexture()` again. Already fixed in current code.

### VizDisplayCompositor Crash (CEF 145+)

**Symptom**: GPU process crashes on startup.
**Cause**: `disable-features=VizDisplayCompositor` was set as a chromium flag. This feature flag is incompatible with CEF 145+.
**Mitigation**: Flag was removed. **Do not add it back.**

### loadURL() Ignored

**Symptom**: Calling `loadURL()` with a new URL has no effect.
**Workaround**: Use `executeJavascript("location.reload()")` for content reload. Ctrl+R already uses this approach.

### DDP LED Panel Flashing During Static Content

**Symptom**: LED panels connected via DDP (network LED controller protocol) flash/go dark every 1-2 seconds when page content is static.
**Cause**: CEF throttles/stops delivering paint callbacks when page content doesn't change (OSR optimization). The DDP output module was relying on `OnFrame` being called to resend keepalive data. Without periodic frames, the DDP controller times out and blanks the display.
**Fix**: `DdpOutput` now runs a background keepalive thread that independently resends the last known frame every 100 ms during static content (detected when no new frames arrive). Animation is unaffected — the keepalive is silent when `lastSendTimeMs_` is constantly refreshed by incoming frames.
**Implementation**: `KeepaliveLoop()` in `native/frame-output/protocols/ddp/ddp_output.cpp`. Runs in its own thread, wakes every 100 ms, holds `sendMutex_` during sends to coordinate with `OnFrame`. Interval is tuned to 100 ms (10 Hz) — matches thread wake frequency and controller expectations.
**Related**: [ddp_output.cpp](../native/frame-output/protocols/ddp/ddp_output.cpp)

### FFI Callback Strings Are Garbage (Silent Failure)

**Symptom**: C++ events fire correctly (visible in logs) but JS-side handlers never trigger. `did-navigate` doesn't dispatch, debug panel doesn't inject, keyboard shortcuts may not reach JS. No errors thrown — everything _appears_ to work on the native side.
**Cause**: `threadsafe: true` JSCallbacks with `FFIType.cstring` args deliver raw pointers (numbers), not JS strings. If the callback parameter is typed as `string` and handled with `typeof str === "string"`, the pointer number gets coerced to a garbage string like `"140234567890"`.
**Mitigation**: Always receive cstring args as `number`, then wrap with `new CString(ptr as unknown as Pointer).toString()`. See [ffi-patterns.md](references/ffi-patterns.md#critical-cstring-handling-in-threadsafe-jscallbacks).
**Detection**: Add a temporary `console.log` in the callback to inspect the raw value. If it's a large number instead of a string like `"did-navigate"`, the pattern is wrong.

## Resilience Patterns

| Pattern | Implementation |
|---|---|
| GPU preference registry | Written at startup for all helper executables; takes effect next launch |
| Display window reset | Ctrl+Shift+M destroys and recreates all windows + D3D output |
| Mode toggle | Ctrl+M switches between interactive (master visible) and output (NDWs visible) |
| Clean shutdown | Escape handler stops Spout, destroys display windows, calls `process.exit(0)` |
| Auto-detect fallback | No `display-config.json` → auto-detect displays and create simulation mode for single-display dev |

## Gaps

- No automated watchdog for GPU process health
- No automated display topology change detection (manual Ctrl+Shift+M required)
- No crash logging to disk
- No automatic restart on unrecoverable failure

## Related

- [QUALITY_SCORE.md](QUALITY_SCORE.md) — Quality rubric
- [SECURITY.md](SECURITY.md) — Sandbox and permissions
- [DEVELOPMENT.md](DEVELOPMENT.md) — GPU notes and known issues
