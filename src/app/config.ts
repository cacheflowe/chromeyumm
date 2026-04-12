import { readFileSync, existsSync } from "fs";
import { join, dirname } from "path";

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

export interface SourceRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface WindowConfig {
  slot: number;
  window: { x: number; y: number; width: number; height: number };
  source?: SourceRect;
  label?: string;
}

export interface SpoutOutputConfig {
  senderName: string;
}

export interface SpoutInputConfig {
  senderName: string;
}

export interface DdpOutputConfig {
  controllerAddress: string;
  port?: number;
  destinationId?: number;
  pixelStart?: number;
  source: SourceRect;
  zigZagRows?: boolean;
  flipH?: boolean;
  flipV?: boolean;
  rotate?: 0 | 90 | 180 | 270;
  label?: string;
  enabled?: boolean;
}

export interface DisplayConfig {
  virtualCanvas?: { width: number; height: number };
  windows: WindowConfig[];
  startInteractive?: boolean;
  interactiveWindows?: boolean;
  fullscreen?: boolean;
  alwaysOnTop?: boolean;
  contentUrl?: string | null;
  spoutOutput?: SpoutOutputConfig;
  spoutInput?: SpoutInputConfig;
  ddpOutput?: DdpOutputConfig;
  ddpOutputs?: DdpOutputConfig[];
}

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

function findConfigFile(startDir: string): string | null {
  let dir = startDir;
  while (true) {
    const candidate = join(dir, "display-config.json");
    if (existsSync(candidate)) return candidate;
    const parent = dirname(dir);
    if (parent === dir) return null;
    dir = parent;
  }
}

export function loadDisplayConfig(): DisplayConfig | null {
  const configPath = findConfigFile(process.cwd());
  if (!configPath) return null;

  console.log(`[chromeyumm] Found display-config.json at: ${configPath}`);

  try {
    const text = readFileSync(configPath, "utf-8");
    const config = JSON.parse(text) as DisplayConfig;

    if (!Array.isArray(config.windows) || config.windows.length === 0) {
      console.error("[chromeyumm] display-config.json: 'windows' must be a non-empty array.");
      return null;
    }
    return config;
  } catch (err) {
    console.error("[chromeyumm] Failed to parse display-config.json:", err);
    return null;
  }
}

export function resolveVirtualCanvas(config: DisplayConfig): { width: number; height: number } {
  if (config.virtualCanvas) return config.virtualCanvas;

  const minX = Math.min(...config.windows.map((w) => w.window.x));
  const width = Math.max(...config.windows.map((w) => w.window.x + w.window.width)) - minX;
  const height = Math.max(...config.windows.map((w) => w.window.height));
  return { width, height };
}
