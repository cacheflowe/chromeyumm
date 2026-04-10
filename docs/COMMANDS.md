# Commands

All commands run from the project root unless noted.

## Build

| Command | Description |
|---|---|
| `bun install` | Install dependencies (first time only) |
| `bun run build` | Full rebuild: compile C++ DLL + bundle TS + compile `chromeyumm.exe` + copy CEF runtime to `dist/` |
| `bun run build:dev` | Full build with no minification and inline sourcemaps |
| `bun run build:ts` | TS bundle + exe compile only (no MSVC). Fast for TS-only changes. |
| `bun build.ts --dev --skip-native` | TS-only build, unminified with sourcemaps |

## Run

| Command | Description |
|---|---|
| `bun start` | Run compiled exe from project root |
| `dist/chromeyumm.exe` | Run compiled exe directly |
| `bun run demo` | Start R3F Vite dev server + chromeyumm browser together |

## Vendor Setup (first time / new machine)

```bash
bun scripts/setup-vendors.ts           # download + build CEF + Spout vendors
bun scripts/setup-vendors.ts --cef-only
bun scripts/setup-vendors.ts --spout-only
bun scripts/setup-vendors.ts --cef-version "VERSION"  # override CEF version
bun scripts/setup-vendors.ts --spout-tag "TAG"         # override Spout tag
```

See `native/README.md` for vendor layout details and `docs/references/vendor-management.md` for upgrade strategy.

## CEF Wrapper Library Build

```powershell
cd native/vendor/cef
mkdir build; cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release --target libcef_dll_wrapper
```

## R3F View Build

```powershell
cd src/views/r3f
bun install
bun run build    # outputs to src/views/r3f/dist/
```

## Debugging

| Command | Description |
|---|---|
| `dumpbin /exports dist/libNativeWrapper.dll` | Verify DLL export names |
| Ctrl+D (at runtime) | Toggle debug panel (pages must include debug-panel.js) |

## Release

| Command | Description |
|---|---|
| `bun scripts/release.ts` | Build + package `release/chromeyumm-v{VERSION}-win-x64.zip` |
| `bun scripts/release.ts --skip-build` | Package existing `dist/` without rebuilding |
| `bun scripts/release.ts --bump patch` | Bump patch version, build, and package |
| `bun scripts/release.ts --bump minor` | Bump minor version, build, and package |
| `bun scripts/release.ts --publish` | Build, package, git tag, and create GitHub release (requires `gh` CLI) |

GitHub Actions: push a `v*` tag to trigger `.github/workflows/release.yml` automatically.

## What Requires Which Build

| Changed | Rebuild needed |
|---|---|
| `native/cef-wrapper.cpp` or `cef-helper.cpp` | Full (`bun build.ts`) |
| Anything in `src/chromeyumm/` | `bun build.ts --skip-native` |
| Anything in `src/app/` | `bun build.ts --skip-native` |
| `display-config.json` only | No rebuild — found via cwd walk-up at runtime |
| `native/app.ico` | Full (`bun build.ts`) — copies icon to `dist/` |
| External `contentUrl` (`http://localhost:5173`) | No rebuild |
| `src/views/r3f/` source | `cd src/views/r3f && bun run build` (served via dev server, not bundled into dist) |

## Related

- [AGENTS.md](../AGENTS.md) — Quick start
- [DEVELOPMENT.md](../DEVELOPMENT.md) — Build workflow details
- [docs/references/cef-upgrade.md](references/cef-upgrade.md) — CEF version upgrade procedure
- [docs/references/vendor-management.md](references/vendor-management.md) — Vendor dependency strategy
