import { BrowserWindow, NativeDisplayWindow, Screen, GlobalShortcut } from "chromeyumm";
import { dirname, join } from "path";
import { loadDisplayConfig, resolveVirtualCanvas } from "./config.ts";
import { SpoutInput } from "./spout-input.ts";

// ---------------------------------------------------------------------------
// GPU preference — ensure CEF helper processes use the high-performance GPU.
// Writes registry entries at:  HKCU\Software\Microsoft\DirectX\UserGpuPreferences
// Takes effect on the NEXT launch after first run.
// ---------------------------------------------------------------------------
(function setGpuPreference() {
	const regKey = "HKCU\\Software\\Microsoft\\DirectX\\UserGpuPreferences";
	const binDir = dirname(process.execPath);
	const helperNames = [
		"bun.exe",
		"bun Helper.exe",
		"bun Helper (GPU).exe",
		"bun Helper (Renderer).exe",
		"bun Helper (Alerts).exe",
		"bun Helper (Plugin).exe",
	];
	for (const name of helperNames) {
		const exe = join(binDir, name);
		const result = Bun.spawnSync(
			["reg", "add", regKey, "/v", exe, "/t", "REG_SZ", "/d", "GpuPreference=2;", "/f"],
			{ stderr: "ignore", stdout: "ignore" },
		);
		if (result.exitCode === 0) console.log(`[chromeyumm] GPU preference registered: ${exe}`);
		else console.warn(`[chromeyumm] GPU preference registration failed: ${exe}`);
	}
})();

// ---------------------------------------------------------------------------
// Scripts injected into the master webview
// ---------------------------------------------------------------------------
const HIDE_CURSOR_SCRIPT   = `document.documentElement.style.setProperty('cursor','none','important')`;
const SHOW_CURSOR_SCRIPT   = `document.documentElement.style.removeProperty('cursor')`;
const TOGGLE_PANEL_SCRIPT  = `(function(){if(typeof window.__ebPanelToggle==='function')window.__ebPanelToggle();})()`;
const TOGGLE_OVERLAY_SCRIPT= `(function(){if(typeof window.__ebDebugToggle==='function')window.__ebDebugToggle();})()`;

// ---------------------------------------------------------------------------
// Config / layout
// ---------------------------------------------------------------------------

const config = loadDisplayConfig();

interface DisplaySlot {
	slot: number;
	windowX: number; windowY: number; windowW: number; windowH: number;
	sourceX: number; sourceY: number; sourceWidth: number; sourceHeight: number;
	simulated: boolean;
}

let slots: DisplaySlot[];
let contentUrl: string | null = null;
let fullscreen   = false;
let alwaysOnTop  = false;
let totalWidth:  number;
let totalHeight: number;

if (config) {
	console.log(`[chromeyumm] Loaded display-config.json — ${config.windows.length} window(s).`);

	contentUrl  = config.contentUrl ?? null;
	fullscreen  = config.fullscreen ?? false;
	alwaysOnTop = config.alwaysOnTop ?? false;

	({ width: totalWidth, height: totalHeight } = resolveVirtualCanvas(config));

	const sorted  = [...config.windows].sort((a, b) => a.slot - b.slot);
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
			sourceX: source.x, sourceY: source.y,
			sourceWidth: source.width, sourceHeight: source.height,
			simulated: false,
			windowX: win.window.x, windowY: win.window.y,
			windowW: win.window.width, windowH: win.window.height,
		};
	});
} else {
	// Auto-detect mode
	const allDisplays  = Screen.getAllDisplays();
	const sorted       = [...allDisplays].sort((a, b) => a.bounds.x - b.bounds.x);
	const SIMULATE     = sorted.length < 2;

	if (SIMULATE) {
		console.log("[chromeyumm] No display-config.json — one display detected, entering simulation mode.");
		const primary = sorted[0]!;
		const W = primary.bounds.width;
		const H = primary.bounds.height;
		const simW = Math.floor(W / 2) - 20;
		const simH = Math.floor(H / 2) - 60;
		totalWidth  = simW * 2;
		totalHeight = simH;
		slots = [
			{ slot: 0, simulated: true, sourceX: 0,    sourceY: 0, sourceWidth: simW, sourceHeight: simH,
			  windowX: primary.bounds.x + 10,       windowY: primary.bounds.y + 80, windowW: simW, windowH: simH },
			{ slot: 1, simulated: true, sourceX: simW, sourceY: 0, sourceWidth: simW, sourceHeight: simH,
			  windowX: primary.bounds.x + simW + 30, windowY: primary.bounds.y + 80, windowW: simW, windowH: simH },
		];
	} else {
		console.log(`[chromeyumm] No display-config.json — auto-detecting ${sorted.length} displays.`);
		totalWidth  = sorted.reduce((s, d) => s + d.bounds.width, 0);
		totalHeight = Math.max(...sorted.map((d) => d.bounds.height));
		let sourceX = 0;
		slots = sorted.map((display, slot) => {
			const entry: DisplaySlot = {
				slot, simulated: false,
				sourceX, sourceY: 0,
				sourceWidth: display.bounds.width, sourceHeight: display.bounds.height,
				windowX: display.bounds.x, windowY: display.bounds.y,
				windowW: display.bounds.width, windowH: display.bounds.height,
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
if (fullscreen)  console.log("[chromeyumm] fullscreen: true");
if (alwaysOnTop) console.log("[chromeyumm] alwaysOnTop: true");
console.log(
	`[chromeyumm] Mode: ${config?.spoutOutput ? "spout output" : "multi-window D3D output"} / ANGLE: d3d11`,
);

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

const _baseUrl = contentUrl ?? "views://threejs/index.html";
const urlParams: string[] = [
	`totalWidth=${totalWidth}`,
	`totalHeight=${totalHeight}`,
];
if (spoutInput) urlParams.push(`spoutReceiverId=${spoutInput.id}`);
if (config?.spoutInput)  urlParams.push(`spoutInputName=${encodeURIComponent(config.spoutInput.senderName)}`);
if (config?.spoutOutput) urlParams.push(`spoutOutputName=${encodeURIComponent(config.spoutOutput.senderName)}`);
if (!config?.spoutOutput)
	urlParams.push(
		`slots=${encodeURIComponent(JSON.stringify(
			slots.map((s) => ({ slot: s.slot, x: s.sourceX, y: s.sourceY, w: s.sourceWidth, h: s.sourceHeight, sim: s.simulated })),
		))}`,
	);

const masterUrl = `${_baseUrl}${_baseUrl.includes("?") ? "&" : "?"}${urlParams.join("&")}`;

const masterX = Math.min(...slots.map((s) => s.windowX));
const masterY = Math.min(...slots.map((s) => s.windowY));

const useD3DOutput = !config?.spoutOutput;

const master = new BrowserWindow({
	title: "chromeyumm",
	url: masterUrl,
	spout: true,   // always OSR — Spout mode sends to SpoutDX; D3D output blits to NDWs
	frame: { x: masterX, y: masterY, width: totalWidth, height: totalHeight },
	viewsRoot: process.cwd(),
});

console.log(`[chromeyumm] Master window id=${master.id} at (${masterX},${masterY}), size=${totalWidth}×${totalHeight}`);

if (config?.spoutOutput) {
	const ok = master.startSpout(config.spoutOutput.senderName);
	if (ok) console.log(`[chromeyumm] Spout sender started: "${config.spoutOutput.senderName}"`);
	else    console.warn("[chromeyumm] Spout sender failed — is SpoutDX built into the DLL?");
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

if (!config?.spoutOutput) {
	createDisplayWindows();
}

// ---------------------------------------------------------------------------
// Interactive mode
// ---------------------------------------------------------------------------

let interactiveMode = config?.startInteractive ?? false;

if (useD3DOutput) {
	if (interactiveMode) {
		displayWindows.forEach((w) => w.setVisible(false));
	} else {
		master.hide();
	}
}

master.webview.on("dom-ready", () => {
	if (!interactiveMode) master.webview.executeJavascript(HIDE_CURSOR_SCRIPT);
	master.webview.executeJavascript(`window.__ebState=${JSON.stringify({ alwaysOnTop, interactiveMode })}`);
});

if (!config?.spoutOutput) {
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
		master.webview.executeJavascript(`(window.__ebState=window.__ebState||{}).interactiveMode=${interactiveMode}`);
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
	master.webview.executeJavascript(`(window.__ebState=window.__ebState||{}).alwaysOnTop=${alwaysOnTop}`);
	console.log(`[chromeyumm] Ctrl+F: alwaysOnTop → ${alwaysOnTop}`);
});

GlobalShortcut.register("CommandOrControl+D", () => {
	console.log("[chromeyumm] Ctrl+D: toggling debug panel");
	master.webview.executeJavascript(TOGGLE_PANEL_SCRIPT);
	master.webview.executeJavascript(TOGGLE_OVERLAY_SCRIPT);
});

GlobalShortcut.register("Escape", () => {
	console.log("[chromeyumm] ESC: quitting");
	if (config?.spoutOutput) master.stopSpout();
	else destroyDisplayWindows();
	if (spoutInput) spoutInput.stop();
	process.exit(0);
});
