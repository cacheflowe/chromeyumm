# native/

This directory holds the C++ source and vendor libraries for the Chromeyumm CEF wrapper.

## Source files (committed)

| File | Origin | Description |
|---|---|---|
| `cef-wrapper.cpp` | `package/src/native/win/nativeWrapper.cpp` | Main CEF/D3D/Spout wrapper DLL |
| `cef-helper.cpp`  | `package/src/native/win/cef_process_helper_win.cpp` | CEF renderer helper (Spout input V8 bindings) |
| `shared/`         | `package/src/native/shared/` (subset) | Cross-platform headers used by cef-wrapper.cpp |

### Files to copy from the Electrobun repo

```
# From package/src/native/win/
cp nativeWrapper.cpp          ../chrome-yumm/native/cef-wrapper.cpp
cp cef_process_helper_win.cpp ../chrome-yumm/native/cef-helper.cpp

# Shared headers (subset — see list below)
cp -r package/src/native/shared/ ../chrome-yumm/native/shared/
```

#### Shared headers to keep

- `callbacks.h`           — FFI callback type definitions
- `chromium_flags.h`      — CEF command-line flag parsing
- `cef_response_filter.h` — HTTP response filter (preload injection)
- `navigation_rules.h`    — URL navigation filtering
- `permissions.h`         — sandbox permission rules
- `mime_types.h`          — views:// MIME type map
- `preload_script.h`      — injected JS preload
- `thread_safe_map.h`     — utility
- `shutdown_guard.h`      — safe shutdown coordination
- `glob_match.h`          — utility
- `app_paths.h`           — app directory resolution (no ASAR needed)
- `accelerator_parser.h`  — keyboard shortcut parsing
- `ffi_helpers.h`         — FFI marshal helpers
- `config.h`              — config structs
- `pending_resize_queue.h`— OSR resize queue

#### Shared headers to skip

- `json_menu_parser.h`  — native menus (not used)
- `webview_storage.h`   — WebView2-only
- `download_event.h`    — optional, keep if you want download events

## Vendor directories (NOT committed — supply separately)

### CEF  (`native/vendor/cef/`)

Download a CEF prebuilt from: https://cef-builds.spotifycdn.com/index.html

Target: `Windows 64-bit` → `Standard Distribution`

Required layout after extraction:
```
native/vendor/cef/
  include/          ← CEF C++ headers
  Release/
    libcef.dll
    libcef.lib
    chrome_elf.dll
    d3dcompiler_47.dll
    libEGL.dll
    libGLESv2.dll
    snapshot_blob.bin
    v8_context_snapshot.bin
    (+ other runtime files)
  Resources/
    icudtl.dat
    chrome_100_percent.pak
    chrome_200_percent.pak
    resources.pak
    locales/
  build/
    libcef_dll_wrapper/
      Release/
        libcef_dll_wrapper.lib
```

The `libcef_dll_wrapper.lib` must be built from the CEF CMake project:
```
cd native/vendor/cef
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release --target libcef_dll_wrapper
```

### Spout  (`native/vendor/spout/`) — optional

Required only if building with Spout output support.

Download SpoutDX from: https://github.com/leadedge/Spout2

Build or obtain:
```
native/vendor/spout/
  include/
    SpoutDX/
      SpoutDX.h
      SpoutCommon.h
      (other SpoutDX headers)
  MT/
    lib/
      SpoutDX_static.lib   ← must be /MT (static CRT) to match build flags
```

If `SpoutDX_static.lib` is absent, `build.ts` builds without Spout support
(`#ifdef ELECTROBUN_HAS_SPOUT` guard in cef-wrapper.cpp).

## CEF version upgrades

1. Download the new CEF prebuilt (same layout as above).
2. Replace `native/vendor/cef/` contents.
3. Rebuild `libcef_dll_wrapper.lib` (CMake step above).
4. Run `bun build.ts`.
5. Test for API breakage — the CEF C++ API (`OnAcceleratedPaint`,
   `CefBrowserSettings`, etc.) is very stable; breaking changes are rare.
   Check the CEF changelog at: https://bitbucket.org/chromiumembedded/cef/wiki/Home

## Helper EXEs

CEF spawns helper processes named after the host executable:
- `bun Helper.exe`
- `bun Helper (GPU).exe`
- `bun Helper (Renderer).exe`
- `bun Helper (Alerts).exe`
- `bun Helper (Plugin).exe`

These names are derived from `bun.exe` (the Bun runtime). The GPU preference
registry entries in `src/app/index.ts` target these exact names.

If you rename the host binary (e.g. to `chromeyumm.exe`), update both:
1. The `helperNames` array in `src/app/index.ts`
2. Any CEF subprocess path configuration in `cef-wrapper.cpp`
