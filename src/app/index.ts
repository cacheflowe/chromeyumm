import { BrowserWindow, NativeDisplayWindow, Screen, GlobalShortcut } from "chromeyumm";
import { dirname, join, basename, isAbsolute, resolve } from "path";
import { readFileSync, existsSync } from "fs";
import { loadDisplayConfig, resolveVirtualCanvas } from "./config.ts";
import { SpoutInput } from "./spout-input.ts";
import { native, cs } from "../chromeyumm/ffi.ts";
import { CEF_VERSION } from "./_cef-version.ts";

// ---------------------------------------------------------------------------
// Start the Windows message loop + CEF on a background thread.
// Must happen before any window/webview creation (which use dispatch_sync).
// ---------------------------------------------------------------------------
native.symbols.initEventLoop(cs("chromeyumm"), cs("Chromeyumm"), cs("stable"));

// ---------------------------------------------------------------------------
// GPU preference — ensure CEF helper processes use the high-performance GPU.
// Writes registry entries at:  HKCU\Software\Microsoft\DirectX\UserGpuPreferences
// Takes effect on the NEXT launch after first run.
// ---------------------------------------------------------------------------
(function setGpuPreference() {
  const regKey = "HKCU\\Software\\Microsoft\\DirectX\\UserGpuPreferences";
  const binDir = dirname(process.execPath);
  const exeStem = basename(process.execPath, ".exe"); // "chromeyumm" or "bun"
  const helperNames = [
    basename(process.execPath), // e.g. "chromeyumm.exe" or "bun.exe"
    `${exeStem} Helper.exe`, // e.g. "chromeyumm Helper.exe"
  ];
  for (const name of helperNames) {
    const exe = join(binDir, name);
    const result = Bun.spawnSync(["reg", "add", regKey, "/v", exe, "/t", "REG_SZ", "/d", "GpuPreference=2;", "/f"], {
      stderr: "ignore",
      stdout: "ignore",
    });
    if (result.exitCode === 0) console.log(`[chromeyumm] GPU preference registered: ${exe}`);
    else console.warn(`[chromeyumm] GPU preference registration failed: ${exe}`);
  }
})();

// ---------------------------------------------------------------------------
// Scripts injected into the master webview
// ---------------------------------------------------------------------------
const HIDE_CURSOR_SCRIPT = `document.documentElement.style.setProperty('cursor','none','important')`;
const SHOW_CURSOR_SCRIPT = `document.documentElement.style.removeProperty('cursor')`;
const TOGGLE_PANEL_SCRIPT = `window.__chromeyummToggle?.()`;

// Auto-inject script — bundled from src/components/inject.js at build time.
// Injects <debug-panel> into any page that doesn't already have one.
let debugInjectScript = "";
{
  // Try exe directory first (production), then dist/ relative to cwd (dev mode)
  const candidates = [
    join(dirname(process.execPath), "debug-inject.js"),
    join(process.cwd(), "dist", "debug-inject.js"),
  ];
  for (const path of candidates) {
    try {
      debugInjectScript = readFileSync(path, "utf-8");
      break;
    } catch {}
  }
  if (!debugInjectScript) {
    console.warn("[chromeyumm] debug-inject.js not found — debug panel auto-injection disabled");
  }
}

// ---------------------------------------------------------------------------
// Config / layout
// ---------------------------------------------------------------------------

const featureCheckMode = process.argv.includes("--feature-check");
const config = featureCheckMode ? null : loadDisplayConfig();
const configuredDdpOutputs = [...(config?.ddpOutput ? [config.ddpOutput] : []), ...(config?.ddpOutputs ?? [])].filter(
  (ddp) => ddp.enabled !== false,
);

interface DisplaySlot {
  slot: number;
  windowX: number;
  windowY: number;
  windowW: number;
  windowH: number;
  sourceX: number;
  sourceY: number;
  sourceWidth: number;
  sourceHeight: number;
  simulated: boolean;
}

let slots: DisplaySlot[];
let contentUrl: string | null = null;
let fullscreen = false;
let alwaysOnTop = false;
let totalWidth: number;
let totalHeight: number;

if (featureCheckMode) {
  // --feature-check: single centered window, no Spout/D3D, feature-check page
  const fcPath = resolve(process.cwd(), "src/views/feature-check/index.html");
  if (!existsSync(fcPath)) {
    console.error(`[chromeyumm] Feature check page not found: ${fcPath}`);
    process.exit(1);
  }
  contentUrl = "file:///" + fcPath.replace(/\\/g, "/");
  totalWidth = 900;
  totalHeight = 800;
  // Use CW_USEDEFAULT-style placement (top-left offset); Screen may not
  // be available this early when running via `bun src/app/index.ts`.
  const displays = Screen.getAllDisplays();
  const primary = displays[0];
  const cx = primary ? primary.bounds.x + Math.floor((primary.bounds.width - totalWidth) / 2) : 100;
  const cy = primary ? primary.bounds.y + Math.floor((primary.bounds.height - totalHeight) / 2) : 100;
  slots = [
    {
      slot: 0,
      simulated: true,
      sourceX: 0,
      sourceY: 0,
      sourceWidth: totalWidth,
      sourceHeight: totalHeight,
      windowX: cx,
      windowY: cy,
      windowW: totalWidth,
      windowH: totalHeight,
    },
  ];
  console.log("[chromeyumm] Feature check mode — single 900×800 window");
} else if (config) {
  console.log(`[chromeyumm] Loaded display-config.json — ${config.windows.length} window(s).`);

  contentUrl = config.contentUrl ?? null;

  // Resolve relative contentUrl paths to file:// URLs.
  // e.g. "src/views/feature-check/index.html" → "file:///D:/workspace/chromeyumm/src/views/..."
  if (contentUrl && !contentUrl.includes("://")) {
    const abs = isAbsolute(contentUrl) ? contentUrl : resolve(process.cwd(), contentUrl);
    if (existsSync(abs)) {
      contentUrl = "file:///" + abs.replace(/\\/g, "/");
    } else {
      console.warn(`[chromeyumm] contentUrl path not found: ${abs}`);
    }
  }
  fullscreen = config.fullscreen ?? false;
  alwaysOnTop = config.alwaysOnTop ?? false;

  ({ width: totalWidth, height: totalHeight } = resolveVirtualCanvas(config));

  const sorted = [...config.windows].sort((a, b) => a.slot - b.slot);
  const minWinX = Math.min(...sorted.map((w) => w.window.x));
  const minWinY = Math.min(...sorted.map((w) => w.window.y));

  slots = sorted.map((win) => {
    const source = win.source ?? {
      x: win.window.x - minWinX,
      y: win.window.y - minWinY,
      width: win.window.width,
      height: win.window.height,
    };
    return {
      slot: win.slot,
      sourceX: source.x,
      sourceY: source.y,
      sourceWidth: source.width,
      sourceHeight: source.height,
      simulated: false,
      windowX: win.window.x,
      windowY: win.window.y,
      windowW: win.window.width,
      windowH: win.window.height,
    };
  });
} else {
  // Auto-detect mode
  const allDisplays = Screen.getAllDisplays();
  const sorted = [...allDisplays].sort((a, b) => a.bounds.x - b.bounds.x);
  const SIMULATE = sorted.length < 2;

  if (SIMULATE) {
    console.log("[chromeyumm] No display-config.json — one display detected, entering simulation mode.");
    const primary = sorted[0]!;
    const W = primary.bounds.width;
    const H = primary.bounds.height;
    const simW = Math.floor(W / 2) - 20;
    const simH = Math.floor(H / 2) - 60;
    totalWidth = simW * 2;
    totalHeight = simH;
    slots = [
      {
        slot: 0,
        simulated: true,
        sourceX: 0,
        sourceY: 0,
        sourceWidth: simW,
        sourceHeight: simH,
        windowX: primary.bounds.x + 10,
        windowY: primary.bounds.y + 80,
        windowW: simW,
        windowH: simH,
      },
      {
        slot: 1,
        simulated: true,
        sourceX: simW,
        sourceY: 0,
        sourceWidth: simW,
        sourceHeight: simH,
        windowX: primary.bounds.x + simW + 30,
        windowY: primary.bounds.y + 80,
        windowW: simW,
        windowH: simH,
      },
    ];
  } else {
    console.log(`[chromeyumm] No display-config.json — auto-detecting ${sorted.length} displays.`);
    totalWidth = sorted.reduce((s, d) => s + d.bounds.width, 0);
    totalHeight = Math.max(...sorted.map((d) => d.bounds.height));
    let sourceX = 0;
    slots = sorted.map((display, slot) => {
      const entry: DisplaySlot = {
        slot,
        simulated: false,
        sourceX,
        sourceY: 0,
        sourceWidth: display.bounds.width,
        sourceHeight: display.bounds.height,
        windowX: display.bounds.x,
        windowY: display.bounds.y,
        windowW: display.bounds.width,
        windowH: display.bounds.height,
      };
      sourceX += display.bounds.width;
      return entry;
    });
  }
}

console.log(`[chromeyumm] Virtual canvas: ${totalWidth}×${totalHeight}`);
slots.forEach((s) =>
  console.log(
    `  Slot ${s.slot}: window=(${s.windowX},${s.windowY} ${s.windowW}×${s.windowH})  ` +
      `source=(${s.sourceX},${s.sourceY} ${s.sourceWidth}×${s.sourceHeight})` +
      (s.simulated ? "  [simulated]" : ""),
  ),
);
if (contentUrl) console.log(`[chromeyumm] Content URL: ${contentUrl}`);
if (fullscreen) console.log("[chromeyumm] fullscreen: true");
if (alwaysOnTop) console.log("[chromeyumm] alwaysOnTop: true");
const useD3DOutput = slots.length > 0;
const modeSpout = !!config?.spoutOutput;
const modeDdp = configuredDdpOutputs.length > 0;
const modeMultiWindow = useD3DOutput;
const modeParts = [
  modeSpout ? `spout → "${config!.spoutOutput!.senderName}"` : null,
  modeDdp ? `ddp (${configuredDdpOutputs.length} output${configuredDdpOutputs.length !== 1 ? "s" : ""})` : null,
  modeMultiWindow ? `multi-window D3D (${slots.length} slot${slots.length !== 1 ? "s" : ""})` : null,
].filter(Boolean);
console.log(`[chromeyumm] Mode: ${modeParts.join(" + ") || "headless"} / ANGLE: d3d11`);

// ---------------------------------------------------------------------------
// Spout input — start BEFORE BrowserWindow so the shared-memory mapping
// is ready when OnContextCreated fires in the CEF renderer.
// ---------------------------------------------------------------------------

let spoutInput: SpoutInput | null = null;
if (config?.spoutInput) {
  spoutInput = new SpoutInput(config.spoutInput.senderName);
  if (!spoutInput.start()) spoutInput = null;
}

// ---------------------------------------------------------------------------
// Master BrowserWindow
// ---------------------------------------------------------------------------

if (!contentUrl) {
  // No contentUrl configured — show built-in welcome page with setup instructions.
  const welcomeCandidates = [
    join(dirname(process.execPath), "views", "welcome", "index.html"),
    join(process.cwd(), "dist", "views", "welcome", "index.html"),
    join(process.cwd(), "src", "views", "welcome", "index.html"),
  ];
  const welcomePath = welcomeCandidates.find((p) => existsSync(p));
  if (welcomePath) {
    contentUrl = "file:///" + welcomePath.replace(/\\/g, "/");
    console.log("[chromeyumm] No contentUrl — showing welcome page");
  } else {
    console.error(
      "[chromeyumm] No contentUrl in display-config.json and welcome page not found.",
    );
    process.exit(1);
  }
}
const _baseUrl = contentUrl;
const urlParams: string[] = [`totalWidth=${totalWidth}`, `totalHeight=${totalHeight}`];
if (spoutInput) urlParams.push(`spoutReceiverId=${spoutInput.id}`);
if (config?.spoutInput) urlParams.push(`spoutInputName=${encodeURIComponent(config.spoutInput.senderName)}`);
if (config?.spoutOutput) urlParams.push(`spoutOutputName=${encodeURIComponent(config.spoutOutput.senderName)}`);
if (!config?.spoutOutput)
  urlParams.push(
    `slots=${encodeURIComponent(
      JSON.stringify(
        slots.map((s) => ({
          slot: s.slot,
          x: s.sourceX,
          y: s.sourceY,
          w: s.sourceWidth,
          h: s.sourceHeight,
          sim: s.simulated,
        })),
      ),
    )}`,
  );

const masterUrl = `${_baseUrl}${_baseUrl.includes("?") ? "&" : "?"}${urlParams.join("&")}`;

const masterX = Math.min(...slots.map((s) => s.windowX));
const masterY = Math.min(...slots.map((s) => s.windowY));

const master = new BrowserWindow({
  title: featureCheckMode ? "Chromeyumm Feature Check" : "chromeyumm",
  url: masterUrl,
  spout: true, // always OSR — Spout mode sends to SpoutDX; D3D output blits to NDWs
  frame: { x: masterX, y: masterY, width: totalWidth, height: totalHeight },
});

console.log(`[chromeyumm] Master window id=${master.id} at (${masterX},${masterY}), size=${totalWidth}×${totalHeight}`);

let activeSpout = false;
if (config?.spoutOutput) {
  activeSpout = master.startSpout(config.spoutOutput.senderName);
  if (activeSpout) console.log(`[chromeyumm] Spout sender started: "${config.spoutOutput.senderName}"`);
  else console.warn("[chromeyumm] Spout sender failed — is SpoutDX built into the DLL?");
}

const activeDdpOutputs: {
  controllerAddress: string;
  port: number;
  destinationId: number;
  pixelStart: number;
  source: { x: number; y: number; width: number; height: number };
  zigZagRows: boolean;
  flipH: boolean;
  flipV: boolean;
  rotate: 0 | 90 | 180 | 270;
  label: string | null;
}[] = [];

if (configuredDdpOutputs.length > 0) {
  configuredDdpOutputs.forEach((ddp, idx) => {
    const port = ddp.port ?? 4048;
    const destinationId = ddp.destinationId ?? 0x01;
    const pixelStart = ddp.pixelStart ?? 0;
    const zigZagRows = ddp.zigZagRows ?? false;
    const flipH = ddp.flipH ?? false;
    const flipV = ddp.flipV ?? false;
    const rotate = ddp.rotate ?? 0;
    const ok = master.startDdpOutput({
      controllerAddress: ddp.controllerAddress,
      port,
      destinationId,
      pixelStart,
      srcX: ddp.source.x,
      srcY: ddp.source.y,
      srcW: ddp.source.width,
      srcH: ddp.source.height,
      zigZagRows,
      flipH,
      flipV,
      rotate,
      clearExisting: idx === 0,
    });

    if (ok) {
      activeDdpOutputs.push({
        controllerAddress: ddp.controllerAddress,
        port,
        destinationId,
        pixelStart,
        source: {
          x: ddp.source.x,
          y: ddp.source.y,
          width: ddp.source.width,
          height: ddp.source.height,
        },
        zigZagRows,
        flipH,
        flipV,
        rotate,
        label: ddp.label ?? null,
      });
      const transformParts = [
        zigZagRows ? "zigzag" : null,
        flipH ? "flipH" : null,
        flipV ? "flipV" : null,
        rotate ? `rot${rotate}` : null,
      ].filter(Boolean);
      console.log(
        `[chromeyumm] DDP output ${idx + 1}/${configuredDdpOutputs.length} started: ` +
          `${ddp.controllerAddress}:${port} dst=${destinationId} start=${pixelStart} ` +
          `src=(${ddp.source.x},${ddp.source.y} ${ddp.source.width}x${ddp.source.height})` +
          (transformParts.length ? ` [${transformParts.join(" ")}]` : ""),
      );
    } else {
      console.warn(
        `[chromeyumm] DDP output ${idx + 1}/${configuredDdpOutputs.length} failed: ` +
          `${ddp.controllerAddress}:${port}`,
      );
    }
  });
}

// ---------------------------------------------------------------------------
// NativeDisplayWindows
// ---------------------------------------------------------------------------

let displayWindows: NativeDisplayWindow[] = [];

function destroyDisplayWindows() {
  if (useD3DOutput) master.stopD3DOutput();
  for (const w of displayWindows) w.destroy();
  displayWindows = [];
}

function createDisplayWindows() {
  if (useD3DOutput) {
    if (!master.startD3DOutput())
      console.warn("[chromeyumm] D3D output failed to start — display windows will be blank");
  }
  for (const slot of slots) {
    const display = new NativeDisplayWindow({
      frame: { x: slot.windowX, y: slot.windowY, width: slot.windowW, height: slot.windowH },
      alwaysOnTop,
      fullscreen,
    });
    console.log(`[chromeyumm] Display window ${display.id} → slot ${slot.slot}`);
    displayWindows.push(display);

    if (useD3DOutput) {
      const ok = master.addD3DOutputSlot(display.id, slot.sourceX, slot.sourceY, slot.sourceWidth, slot.sourceHeight);
      if (!ok) console.warn(`[chromeyumm] addD3DOutputSlot failed for display ${display.id}`);
    }

    // Enable mouse input forwarding on display windows so visitors can
    // interact without switching to interactive mode (Ctrl+M).
    if (config?.interactiveWindows) {
      native.symbols.enableDisplayWindowInput(
        display.id,
        master.webview.id,
        slot.sourceX,
        slot.sourceY,
        slot.sourceWidth,
        slot.sourceHeight,
        true,
        true, // enable, showCursor
      );
    }
  }
}

function resetDisplayWindows() {
  console.log("[chromeyumm] Resetting display windows...");
  destroyDisplayWindows();
  createDisplayWindows();
  if (interactiveMode) {
    displayWindows.forEach((w) => w.setVisible(false));
  } else {
    if (useD3DOutput) master.hide();
    displayWindows.forEach((w) => w.setVisible(true));
  }
  master.webview.executeJavascript("location.reload()");
  console.log("[chromeyumm] Display windows reset complete.");
}

createDisplayWindows();

const SEP = "[chromeyumm] " + "─".repeat(52);
console.log(SEP);
console.log("[chromeyumm]  ACTIVE OUTPUTS");
if (activeSpout) console.log(`[chromeyumm]    Spout        → "${config!.spoutOutput!.senderName}"`);
if (activeDdpOutputs.length > 0) console.log(`[chromeyumm]    DDP          → ${activeDdpOutputs.length} output(s)`);
if (modeMultiWindow)
  console.log(
    `[chromeyumm]    Multi-window → ${displayWindows.length} D3D display window${displayWindows.length !== 1 ? "s" : ""}`,
  );
if (!activeSpout && activeDdpOutputs.length === 0 && !modeMultiWindow) console.log("[chromeyumm]    (none — headless)");
console.log(`[chromeyumm]  Content URL  → ${contentUrl}`);
console.log(SEP);

// ---------------------------------------------------------------------------
// Build the __chromeyumm state object — the API contract between native/app
// and web content. The debug panel reads this on a 1-second poll.
// ---------------------------------------------------------------------------

const outputMode =
  [activeSpout ? "spout" : null, activeDdpOutputs.length > 0 ? "ddp" : null, modeMultiWindow ? "d3d" : null]
    .filter(Boolean)
    .join("+") || "headless";

function getDdpStatsSnapshot() {
  if (activeDdpOutputs.length === 0) return null;
  return master.getDdpOutputStats();
}

function buildChromeyummState() {
  const hotkeys: { key: string; action: string; stateKey?: string }[] = [
    { key: "Ctrl+R", action: "reload" },
    { key: "Ctrl+F", action: "always-on-top", stateKey: "alwaysOnTop" },
  ];
  if (useD3DOutput) {
    hotkeys.push({ key: "Ctrl+M", action: "interactive mode", stateKey: "interactiveMode" });
    hotkeys.push({ key: "Ctrl+Shift+M", action: "reset windows" });
  }
  hotkeys.push({ key: "Ctrl+D", action: "toggle debug panel" });
  hotkeys.push({ key: "F12", action: "toggle DevTools" });
  hotkeys.push({ key: "Escape", action: "quit" });

  return {
    alwaysOnTop,
    interactiveMode,
    display: {
      totalWidth,
      totalHeight,
      contentUrl: contentUrl!,
      fullscreen,
      slots: slots.map((s) => ({
        slot: s.slot,
        x: s.sourceX,
        y: s.sourceY,
        w: s.sourceWidth,
        h: s.sourceHeight,
        sim: s.simulated,
      })),
    },
    output: {
      mode: outputMode,
      d3d: { active: useD3DOutput, windowCount: displayWindows.length },
      spout: {
        active: activeSpout,
        senderName: config?.spoutOutput?.senderName ?? null,
      },
      ddp: {
        active: activeDdpOutputs.length > 0,
        outputCount: activeDdpOutputs.length,
        outputs: activeDdpOutputs,
        stats: getDdpStatsSnapshot(),
      },
    },
    input: {
      spout: {
        active: !!spoutInput,
        senderName: config?.spoutInput?.senderName ?? null,
        receiverId: spoutInput?.id ?? 0,
      },
    },
    hotkeys,
    cefVersion: CEF_VERSION,
  };
}

// ---------------------------------------------------------------------------
// Interactive mode
// ---------------------------------------------------------------------------

let interactiveMode = featureCheckMode || (config?.startInteractive ?? false);

if (useD3DOutput) {
  if (interactiveMode) {
    displayWindows.forEach((w) => w.setVisible(false));
  } else {
    master.hide();
  }
}

master.webview.on("did-navigate", () => {
  if (!interactiveMode) master.webview.executeJavascript(HIDE_CURSOR_SCRIPT);
  // Inject state object (must come before debug panel injection)
  master.webview.executeJavascript(
    `window.__chromeyumm=${JSON.stringify(buildChromeyummState())};window.__ebState=window.__chromeyumm`,
  );
  // Auto-inject <debug-panel> if not already present in the page
  if (debugInjectScript) master.webview.executeJavascript(debugInjectScript);
});

if (useD3DOutput) {
  GlobalShortcut.register("CommandOrControl+M", () => {
    interactiveMode = !interactiveMode;
    if (interactiveMode) {
      displayWindows.forEach((w) => w.setVisible(false));
      if (useD3DOutput) master.show();
      master.webview.executeJavascript(SHOW_CURSOR_SCRIPT);
      console.log("[chromeyumm] Ctrl+M: interactive mode ON");
    } else {
      if (useD3DOutput) master.hide();
      displayWindows.forEach((w) => w.setVisible(true));
      master.webview.executeJavascript(HIDE_CURSOR_SCRIPT);
      console.log("[chromeyumm] Ctrl+M: interactive mode OFF");
    }
    master.webview.executeJavascript(`window.__chromeyumm && (window.__chromeyumm.interactiveMode=${interactiveMode})`);
  });

  GlobalShortcut.register("CommandOrControl+Shift+M", () => {
    console.log("[chromeyumm] Ctrl+Shift+M: resetting display windows");
    resetDisplayWindows();
  });
}

GlobalShortcut.register("CommandOrControl+R", () => {
  console.log("[chromeyumm] Ctrl+R: reloading");
  master.webview.executeJavascript("location.reload()");
});

GlobalShortcut.register("CommandOrControl+F", () => {
  alwaysOnTop = !alwaysOnTop;
  if (config?.spoutOutput) {
    master.setAlwaysOnTop(alwaysOnTop);
  } else {
    displayWindows.forEach((w) => w.setAlwaysOnTop(alwaysOnTop));
  }
  master.webview.executeJavascript(`window.__chromeyumm && (window.__chromeyumm.alwaysOnTop=${alwaysOnTop})`);
  console.log(`[chromeyumm] Ctrl+F: alwaysOnTop → ${alwaysOnTop}`);
});

GlobalShortcut.register("CommandOrControl+D", () => {
  console.log("[chromeyumm] Ctrl+D: toggling debug panel");
  master.webview.executeJavascript(TOGGLE_PANEL_SCRIPT);
});

GlobalShortcut.register("F12", () => {
  console.log("[chromeyumm] F12: toggling DevTools");
  master.webview.toggleDevTools();
});

GlobalShortcut.register("Escape", () => {
  console.log("[chromeyumm] ESC: quitting");
  if (activeDdpOutputs.length > 0) master.stopDdpOutput();
  if (activeSpout) master.stopSpout();
  if (useD3DOutput) destroyDisplayWindows();
  if (spoutInput) spoutInput.stop();
  process.exit(0);
});

// Keep Bun's event loop alive so CEF callbacks (did-navigate, shortcuts, etc.) can fire.
// Without this, Bun exits as soon as top-level JS finishes.
// Poll at 250ms so threadsafe FFI callbacks (hotkeys, CEF events) are processed
// promptly even if the uv_async wakeup from the native thread is missed.
setInterval(() => {}, 250);

if (activeDdpOutputs.length > 0) {
  setInterval(() => {
    const stats = getDdpStatsSnapshot();
    if (!stats) return;
    const json = JSON.stringify(stats);
    master.webview.executeJavascript(
      `window.__chromeyumm && window.__chromeyumm.output && window.__chromeyumm.output.ddp && (window.__chromeyumm.output.ddp.stats=${json})`,
    );
  }, 1000);
}
