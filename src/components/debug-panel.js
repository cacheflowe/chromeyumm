/**
 * debug-panel.js — consolidated debug panel web component for Chromeyumm.
 *
 * Registers <debug-panel> as a custom element. The browser auto-injects it
 * into every page via dist/debug-inject.js (see inject.js). Developers can
 * also place it directly in HTML — the auto-injector skips pages that already
 * have one.
 *
 * ─── WHAT IT HANDLES INTERNALLY ──────────────────────────────────────────────
 *
 *   Keys section     — data-driven from window.__chromeyumm.hotkeys. Shows
 *                      live ON/OFF badges for toggle-type shortcuts.
 *
 *   Display section  — virtual canvas dimensions, slot count, content URL.
 *                      Read from window.__chromeyumm.display.
 *
 *   Output section   — output mode (spout / d3d / spout+d3d / headless),
 *                      D3D window count, Spout sender name.
 *
 *   Spout section    — reads spout input from window.__chromeyumm.input.spout;
 *                      listens to bubbled spout-connect / spout-frame /
 *                      spout-disconnect events from any <spout-receiver> on
 *                      the page for live fps and dimension stats.
 *
 *   Perf section     — FPS, frame-time (ms), and a sparkline of recent frame
 *                      times. Hidden until the first stats.begin() call.
 *
 *   Slot overlay     — coordinate grid + per-slot boundary boxes, rendered
 *                      full-viewport when the panel is open and slot data
 *                      is available. Absorbed from the former <slot-overlay>.
 *
 *   window.__chromeyummToggle — registered for bun's GlobalShortcut Ctrl+D.
 *
 * ─── USAGE ────────────────────────────────────────────────────────────────────
 *
 *   panel.onOpen = () => refresh();   // called immediately on open
 *
 *   // Call whenever you have fresh data (e.g. once per second):
 *   panel.update({
 *     render: "12 draw calls · 45,230 tris",
 *     canvas: "1920×1080 px · css 1920×1080 · dpr 1.00",
 *     mouse:  "(640, 360)",
 *   });
 *
 *   // Wrap your render loop to populate the perf graphs:
 *   const stats = panel.stats;
 *   function animate() {
 *     stats.begin();
 *     renderer.render(scene, camera);
 *     const now = stats.end(); // returns performance.now()
 *   }
 *
 *   // Keys, display, output, and spout sections are populated automatically
 *   // from window.__chromeyumm — do not pass them.
 *   // Use <br> in content strings to produce multiple bullet points.
 */

const SLOT_COLORS = ["#00ffff", "#ffff00", "#ff00ff", "#00ff88", "#ff8800"];
const GRID_PX = 100;

export class DebugPanel extends HTMLElement {
  // ── Instance state ────────────────────────────────────────────────────────

  #content = null; // text-sections div — innerHTML rebuilt on each redraw
  #overlay = null; // full-viewport slot overlay container

  // rAF-based FPS counter — always running, measures actual page frame rate
  #rafId = 0;
  #rafCount = 0;
  #rafLastTime = 0;
  #rafFps = 0;
  #rafMs = 0;
  #rafTick = (now) => {
    this.#rafCount++;
    const delta = now - this.#rafLastTime;
    if (delta >= 500) { // update twice per second for responsiveness
      this.#rafFps = Math.round((this.#rafCount * 1000) / delta);
      this.#rafMs = delta / this.#rafCount;
      this.#rafCount = 0;
      this.#rafLastTime = now;
    }
    this.#rafId = requestAnimationFrame(this.#rafTick);
  };

  // stats.begin()/end() instrumentation — render-loop sparkline only
  static #PERF_HISTORY = 60;
  #perfObj = null;
  #perfUsed = false;
  #perfBegin = 0;
  #perfFrames = []; // ms per render call, max PERF_HISTORY entries

  #onOpen = null;
  #intervalId = 0;

  // Spout stats — maintained via document-level event listeners
  #spoutFrameCount = 0;
  #spoutLastFPSCount = 0;
  #spoutFPS = 0;
  #spoutConnected = false;
  #spoutW = 0;
  #spoutH = 0;

  // Bound handlers stored for removeEventListener pairing
  #onSpoutConnect = (e) => {
    this.#spoutConnected = true;
    this.#spoutW = e.detail.width;
    this.#spoutH = e.detail.height;
    if (this.hasAttribute("open")) this.#redraw();
  };
  #onSpoutFrame = (e) => {
    this.#spoutFrameCount++;
    this.#spoutW = e.detail.width;
    this.#spoutH = e.detail.height;
  };
  #onSpoutDisconnect = () => {
    this.#spoutConnected = false;
    if (this.hasAttribute("open")) this.#redraw();
  };

  // Latest sections pushed by the view
  #viewSections = {};

  // ── Custom element lifecycle ──────────────────────────────────────────────

  connectedCallback() {
    const shadow = this.attachShadow({ mode: "open" });

    const style = document.createElement("style");
    style.textContent = `
      :host {
        position: fixed;
        top: 0; left: 0;
        width: 0; height: 0;
        display: none;
        pointer-events: none;
        z-index: 9999;
      }
      :host([open]) { display: block; }
      .panel {
        position: fixed;
        top: 10px; left: 10px;
        background: rgba(0,0,0,0.85);
        color: rgba(255,255,255,0.82);
        font: 11px/1.7 monospace;
        padding: 10px 14px;
        border-radius: 4px;
        min-width: 300px;
        z-index: 10000;
      }
      .s {
        border-top: 1px solid rgba(255,255,255,0.12);
        padding-top: 5px;
        margin-top: 5px;
      }
      .s:first-child { border-top: none; padding-top: 0; margin-top: 0; }
      .lbl {
        font-size: 9px;
        letter-spacing: 0.08em;
        color: rgba(255,255,255,0.35);
        margin-bottom: 2px;
      }
      ul {
        margin: 0;
        padding: 0 0 0 14px;
        list-style: disc;
      }
      li { margin: 0; padding: 0; }
      li + li { margin-top: 1px; }
      b { font-weight: bold; }
      .overlay {
        position: fixed;
        inset: 0;
        pointer-events: none;
        z-index: 2147483647;
        overflow: hidden;
        font-family: monospace;
      }
    `;

    // Info panel box
    const panelEl = document.createElement("div");
    panelEl.className = "panel";

    // Text sections — rebuilt via innerHTML on each redraw
    this.#content = document.createElement("div");
    panelEl.appendChild(this.#content);

    // Frame timing tracker — exposed via panel.stats; renders inline in #redraw()
    this.#perfObj = {
      begin: () => {
        this.#perfUsed = true;
        this.#perfBegin = performance.now();
      },
      end: () => {
        const now = performance.now();
        this.#perfFrames.push(now - this.#perfBegin);
        if (this.#perfFrames.length > DebugPanel.#PERF_HISTORY) this.#perfFrames.shift();
        return now;
      },
    };

    // Slot overlay container — full viewport, rebuilt on each redraw
    this.#overlay = document.createElement("div");
    this.#overlay.className = "overlay";

    shadow.appendChild(style);
    shadow.appendChild(panelEl);
    shadow.appendChild(this.#overlay);

    // Catch bubbled events from any <spout-receiver> on the page
    document.addEventListener("spout-connect", this.#onSpoutConnect);
    document.addEventListener("spout-frame", this.#onSpoutFrame);
    document.addEventListener("spout-disconnect", this.#onSpoutDisconnect);

    // Toggle function — registered for bun's Ctrl+D shortcut.
    // Both names for backward compat with views that reference __ebPanelToggle.
    window.__chromeyummToggle = window.__ebPanelToggle = () => {
      const opening = !this.hasAttribute("open");
      this.toggleAttribute("open");
      if (opening) {
        this.#redraw();
        if (this.#onOpen) this.#onOpen();
      }
    };

    // Recompute spout fps + redraw once per second
    this.#intervalId = setInterval(() => {
      this.#spoutFPS = this.#spoutFrameCount - this.#spoutLastFPSCount;
      this.#spoutLastFPSCount = this.#spoutFrameCount;
      if (this.hasAttribute("open")) this.#redraw();
    }, 1000);

    // Start always-on rAF FPS counter
    this.#rafLastTime = performance.now();
    this.#rafId = requestAnimationFrame(this.#rafTick);
  }

  disconnectedCallback() {
    clearInterval(this.#intervalId);
    cancelAnimationFrame(this.#rafId);
    document.removeEventListener("spout-connect", this.#onSpoutConnect);
    document.removeEventListener("spout-frame", this.#onSpoutFrame);
    document.removeEventListener("spout-disconnect", this.#onSpoutDisconnect);
  }

  // ── Public API ────────────────────────────────────────────────────────────

  /** Called immediately when the panel opens — use to push fresh view data. */
  set onOpen(fn) {
    this.#onOpen = fn;
  }

  /** True while the panel is visible. */
  get visible() {
    return this.hasAttribute("open");
  }

  /**
   * Render-loop instrumentation. Call begin() before and end() after each render.
   * end() returns performance.now() for reuse as a timestamp.
   * Adds a frame-time sparkline to the perf section (which is always visible via
   * the panel's own rAF counter, regardless of whether stats is used).
   */
  get stats() {
    return this.#perfObj;
  }

  /**
   * Push view-specific section content. Keys, display, output, and spout
   * sections are handled internally; do not pass them here.
   * Use <br> to emit multiple bullets.
   *
   * @param {{
   *   render?: string | null,
   *   canvas?: string | null,
   *   mouse?:  string | null,
   * }} sections
   */
  update(sections) {
    this.#viewSections = { ...sections };
    this.#redraw();
  }

  // ── Rendering helpers ─────────────────────────────────────────────────────

  // Coloured ON / OFF badge.
  static #badge(on) {
    return on ? '<b style="color:#7f7">ON</b>' : '<b style="color:rgba(255,255,255,0.3)">OFF</b>';
  }

  // Wrap a <br>-delimited string in <ul><li> items.
  static #list(str) {
    return (
      "<ul>" +
      str
        .split("<br>")
        .map((s) => `<li>${s.trim()}</li>`)
        .join("") +
      "</ul>"
    );
  }

  // Standard labelled section with bullet list.
  static #section(label, content) {
    return `<div class="s"><div class="lbl">${label}</div>${DebugPanel.#list(content)}</div>`;
  }

  // ── Keys section — data-driven from __chromeyumm.hotkeys ─────────────────

  #buildKeysHTML() {
    const state = window.__chromeyumm || {};
    const hotkeys = state.hotkeys || [];

    if (!hotkeys.length) {
      // Fallback: hardcoded list (e.g. when state object is not yet injected)
      return "<ul><li>Ctrl+D — toggle this panel</li></ul>";
    }

    const items = hotkeys.map((hk) => {
      let line = `${hk.key} — ${hk.action}`;
      if (hk.stateKey && state[hk.stateKey] !== undefined) {
        line += `: ${DebugPanel.#badge(!!state[hk.stateKey])}`;
      }
      return line;
    });

    return "<ul>" + items.map((i) => `<li>${i}</li>`).join("") + "</ul>";
  }

  // ── Display section ───────────────────────────────────────────────────────

  #buildDisplayHTML() {
    const d = (window.__chromeyumm || {}).display;
    if (!d) return null;

    const items = [
      `${d.totalWidth}×${d.totalHeight} virtual canvas`,
      `${d.slots?.length ?? 0} slot${(d.slots?.length ?? 0) !== 1 ? "s" : ""}` + (d.fullscreen ? " · fullscreen" : ""),
    ];

    // Truncate content URL for display
    if (d.contentUrl) {
      const url = d.contentUrl.length > 50 ? d.contentUrl.slice(0, 47) + "..." : d.contentUrl;
      items.push(`<span style="opacity:0.55">${url}</span>`);
    }

    return "<ul>" + items.map((i) => `<li>${i}</li>`).join("") + "</ul>";
  }

  // ── Output section ────────────────────────────────────────────────────────

  #buildOutputHTML() {
    const o = (window.__chromeyumm || {}).output;
    if (!o) return null;

    const items = [`mode: <b>${o.mode}</b>`];

    if (o.d3d?.active) {
      items.push(`D3D: ${o.d3d.windowCount} window${o.d3d.windowCount !== 1 ? "s" : ""}`);
    }
    if (o.spout?.active && o.spout.senderName) {
      items.push(`Spout sender: <b>${o.spout.senderName}</b>`);
    }

    return "<ul>" + items.map((i) => `<li>${i}</li>`).join("") + "</ul>";
  }

  // ── Perf section (frame timing) ──────────────────────────────────────────

  #buildPerfHTML() {
    const fps = this.#rafFps;
    const ms = this.#rafMs.toFixed(1);

    // Render-loop sparkline — only when stats.begin()/end() is in use
    let spark = "";
    if (this.#perfUsed && this.#perfFrames.length > 1) {
      const BARS = "▁▂▃▄▅▆▇█";
      const frames = this.#perfFrames;
      const min = Math.min(...frames);
      const max = Math.max(...frames);
      const range = max - min || 1;
      spark =
        " " +
        frames
          .map((t) => BARS[Math.min(7, Math.floor(((t - min) / range) * 8))])
          .join("");
    }

    return `<ul><li>${fps} fps · ${ms} ms${spark ? `<span style="opacity:0.45;letter-spacing:0.05em">${spark}</span>` : ""}</li></ul>`;
  }

  // ── Memory section ───────────────────────────────────────────────────────

  #buildMemHTML() {
    const mem = performance.memory;
    if (!mem) return null;
    const mb = (b) => (b / 1048576).toFixed(1) + " MB";
    const used = mem.usedJSHeapSize;
    const total = mem.totalJSHeapSize;
    const limit = mem.jsHeapSizeLimit;
    const pct = ((used / limit) * 100).toFixed(0);
    return `<ul><li>${mb(used)} used · ${mb(total)} alloc · ${mb(limit)} limit (${pct}%)</li></ul>`;
  }

  // ── Spout input section ───────────────────────────────────────────────────

  #buildSpoutInputHTML() {
    const inp = (window.__chromeyumm || {}).input?.spout;
    const hasInput = inp?.active || this.#spoutFrameCount > 0;
    if (!hasInput) return null;

    const items = [];
    const name = inp?.senderName || "(unknown)";
    const id = inp?.receiverId ? ` · receiver ${inp.receiverId}` : "";
    items.push(`input sender: <b>${name}</b>${id}`);

    if (this.#spoutFrameCount === 0) {
      items.push(`waiting for frames…`);
    } else if (!this.#spoutConnected) {
      items.push(`<span style="color:#f77">disconnected</span> · ${this.#spoutW}×${this.#spoutH}`);
    } else {
      items.push(`${this.#spoutW}×${this.#spoutH} · ${this.#spoutFPS} fps`);
    }

    const diag = window.__spoutDiag;
    if (diag) items.push(`<span style="opacity:0.55">${diag}</span>`);

    return "<ul>" + items.map((i) => `<li>${i}</li>`).join("") + "</ul>";
  }

  // ── Slot overlay rendering ────────────────────────────────────────────────

  #buildOverlay() {
    this.#overlay.innerHTML = "";
    const state = window.__chromeyumm || {};
    const d = state.display;
    if (!d || !d.slots || !d.slots.length) return;

    const totalWidth = d.totalWidth;
    const totalHeight = d.totalHeight;
    const vpW = window.innerWidth;
    const vpH = window.innerHeight;
    const sx = vpW / totalWidth;
    const sy = vpH / totalHeight;

    const cx = (lx) => (lx * sx).toFixed(2) + "px";
    const cy = (ly) => (ly * sy).toFixed(2) + "px";

    // Coordinate grid
    const grid = document.createElement("div");
    grid.style.cssText = [
      "position:absolute",
      "inset:0",
      "background-image:" +
        "linear-gradient(to right,  rgba(255,255,255,0.10) 1px, transparent 1px)," +
        "linear-gradient(to bottom, rgba(255,255,255,0.10) 1px, transparent 1px)",
      "background-size:" + (GRID_PX * sx).toFixed(2) + "px " + (GRID_PX * sy).toFixed(2) + "px",
    ].join(";");
    this.#overlay.appendChild(grid);

    // X-axis coordinate labels
    for (let x = 0; x < totalWidth; x += GRID_PX) {
      const lbl = document.createElement("span");
      lbl.textContent = x;
      lbl.style.cssText = [
        "position:absolute",
        "left:" + cx(x + 3),
        "top:2px",
        "color:rgba(255,255,255,0.45)",
        "font-size:9px",
        "line-height:1",
        "white-space:nowrap",
      ].join(";");
      this.#overlay.appendChild(lbl);
    }

    // Y-axis coordinate labels
    for (let y = GRID_PX; y < totalHeight; y += GRID_PX) {
      const lbl = document.createElement("span");
      lbl.textContent = y;
      lbl.style.cssText = [
        "position:absolute",
        "left:2px",
        "top:" + cy(y - 11),
        "color:rgba(255,255,255,0.45)",
        "font-size:9px",
        "line-height:1",
        "white-space:nowrap",
      ].join(";");
      this.#overlay.appendChild(lbl);
    }

    // Per-slot boundary boxes + labels
    d.slots.forEach((slot, i) => {
      const color = SLOT_COLORS[i % SLOT_COLORS.length];

      const box = document.createElement("div");
      box.style.cssText = [
        "position:absolute",
        "left:" + cx(slot.x),
        "top:" + cy(slot.y),
        "width:" + cx(slot.w),
        "height:" + cy(slot.h),
        "outline:2px dashed " + color,
        "outline-offset:-2px",
        "box-sizing:border-box",
      ].join(";");
      this.#overlay.appendChild(box);

      const info = document.createElement("div");
      info.innerHTML =
        "<b>SLOT " +
        slot.slot +
        "</b>" +
        (slot.sim ? ' <span style="color:#f80">[sim]</span>' : "") +
        "<br>" +
        slot.w +
        "\u00d7" +
        slot.h +
        " @ (" +
        slot.x +
        "," +
        slot.y +
        ")";
      info.style.cssText = [
        "position:absolute",
        "left:" + cx(slot.x + slot.w / 2),
        "top:" + cy(slot.y + 12),
        "transform:translateX(-50%)",
        "background:rgba(0,0,0,0.6)",
        "color:" + color,
        "font-size:12px",
        "line-height:1.5",
        "padding:4px 8px",
        "border-radius:3px",
        "border:1px solid " + color,
        "text-align:center",
      ].join(";");
      this.#overlay.appendChild(info);
    });
  }

  // ── Full panel render ─────────────────────────────────────────────────────

  #redraw() {
    if (!this.#content || !this.hasAttribute("open")) return;

    const s = this.#viewSections;
    const display = this.#buildDisplayHTML();
    const output = this.#buildOutputHTML();
    const spoutInput = this.#buildSpoutInputHTML();

    let html =
      `<div class="s"><b>debug</b> <span style="opacity:0.4;font-weight:normal">— Ctrl+D to hide</span></div>` +
      `<div class="s"><div class="lbl">keys</div>${this.#buildKeysHTML()}</div>`;

    if (display != null) html += `<div class="s"><div class="lbl">display</div>${display}</div>`;
    if (output != null) html += `<div class="s"><div class="lbl">output</div>${output}</div>`;
    if (s.render != null) html += DebugPanel.#section("render", s.render);
    if (s.canvas != null) html += DebugPanel.#section("canvas", s.canvas);
    if (s.mouse != null) html += DebugPanel.#section("mouse", s.mouse);
    if (spoutInput != null) html += `<div class="s"><div class="lbl">spout input</div>${spoutInput}</div>`;
    const perf = this.#buildPerfHTML();
    if (perf != null) html += `<div class="s"><div class="lbl">perf</div>${perf}</div>`;
    const mem = this.#buildMemHTML();
    if (mem != null) html += `<div class="s"><div class="lbl">mem</div>${mem}</div>`;

    this.#content.innerHTML = html;

    // Rebuild slot overlay
    this.#buildOverlay();
  }
}

if (!customElements.get("debug-panel")) {
  customElements.define("debug-panel", DebugPanel);
}
