# Security

## Threat Model

Chromeyumm runs as a local desktop application for live installations. It is not a general-purpose browser and does not process untrusted web input from the public internet (typical use is loading a trusted dev server URL such as `http://localhost:5173`).

### Attack Surface

| Surface | Risk Level | Notes |
|---|---|---|
| `contentUrl` (local dev server) | Low | Trusted content served from localhost |
| `contentUrl` (external URL) | Medium | If pointed at an untrusted URL, CEF renders arbitrary web content with elevated access |
| `display-config.json` | Low | Local file, not user-facing; parsed with `JSON.parse` (no eval) |
| Registry writes | Low | `GpuPreference=2` written to `HKCU\...\UserGpuPreferences` — benign DirectX preference |
| FFI / dlopen | Low | `libNativeWrapper.dll` loaded from known path next to executable |
| Spout shared memory | Low | Win32 named file mapping accessible to same-user processes |

## CEF Sandbox

### V8 Sandbox

CEF runs with a V8 sandbox that restricts certain operations in the renderer process.

- **Tier 1 Spout input** (`CefV8Value::CreateArrayBuffer` backed by `MapViewOfFile`) requires V8 sandbox OFF. When the sandbox is on, `CreateArrayBuffer` returns nullptr, and the code falls back to tier 2.
- **Tier 2 Spout input** (persistent-buffer `memcpy`) is sandbox-safe.

The current build does NOT explicitly disable the V8 sandbox — tier selection happens automatically at runtime.

### Process Isolation

- `in-process-gpu: true` runs the GPU thread in the main process (required for Optimus, see [RELIABILITY.md](RELIABILITY.md))
- The renderer subprocess (`chromeyumm Helper.exe`) runs in a separate process with CEF's default renderer sandbox

## Registry Writes

At startup, `setGpuPreference()` writes to:
```
HKCU\Software\Microsoft\DirectX\UserGpuPreferences
```

Values written:
- Key: full path to `chromeyumm.exe` → Value: `GpuPreference=2;`
- Key: full path to `chromeyumm Helper.exe` → Value: `GpuPreference=2;`

This uses `reg add /f` (force overwrite). These are standard DirectX user preferences — not a security concern, but worth documenting for auditability.

## Shared Memory (Spout Input)

Win32 named file mappings (`SpoutFrame_<id>`) are created with default security attributes, meaning any process running as the same user can open and read them. This is inherent to Spout's design.

The shared memory region contains only pixel data (BGRA frames) with a 16-byte header (sequence counter, width, height, reserved). No executable code or sensitive data.

## Permissions

- No network server / listening ports (CEF's internal IPC uses named pipes)
- No file system writes beyond the registry preference (CEF cache goes to a temp dir)
- No elevation required (runs as standard user)
- `display-config.json` is read-only from the app's perspective

## Validation

- `display-config.json` validated: `windows` must be a non-empty array, parsed via `JSON.parse` (no eval, no JSONC parser)
- URL parameters sanitized with `encodeURIComponent` before appending to content URL
- Spout sender names taken from config, not user input

## Recommendations

- If deploying with `contentUrl` pointing to an external/untrusted URL, consider enabling CEF's site isolation and renderer sandbox explicitly
- Spout shared memory names should be unpredictable if multi-user scenarios are ever needed (currently not a concern for single-user installation deployments)

## Related

- [RELIABILITY.md](RELIABILITY.md) — Failure modes and recovery
- [BACKEND.md](BACKEND.md) — FFI and DLL loading details
- [DEVELOPMENT.md](../DEVELOPMENT.md) — GPU preference registry details
