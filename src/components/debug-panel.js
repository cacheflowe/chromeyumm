/**
 * debug-panel.js — web component debug panel for Chromeyumm views.
 *
 * Registers <debug-panel> as a custom element. Place directly in HTML:
 *
 *   <debug-panel></debug-panel>
 *
 * then import the module from your script to register the element:
 *
 *   import "../../components/debug-panel.js";
 *   const panel = document.querySelector("debug-panel");
 *
 * ─── WHAT IT HANDLES INTERNALLY ──────────────────────────────────────────────
 *
 *   Keys section   — hardcoded command reference; reads window.__ebState for
 *                    live alwaysOnTop / interactiveMode toggle state.
 *                    bun seeds __ebState on dom-ready and patches it after
 *                    each Ctrl+F / Ctrl+M via executeJavascript.
 *
 *   Spout section  — reads ?spoutOutputName / ?spoutInputName URL params for
 *                    sender names; listens to bubbled spout-connect /
 *                    spout-frame / spout-disconnect events from any
 *                    <spout-receiver> on the page for live fps and dimension
 *                    stats. Shown automatically when either Spout name or
 *                    spoutReceiverId is present in the URL.
 *
 *   Perf section   — stats.js FPS / MS / MB graphs. Hidden until the first
 *                    stats.begin() call, so views without a render loop get
 *                    no spurious empty graphs.
 *
 *   window.__ebPanelToggle — registered for bun's GlobalShortcut Ctrl+D /
 *                    Ctrl+M via executeJavascript.
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
 *     stats.end();
 *   }
 *
 *   // Keys and Spout sections are populated automatically — do not pass them.
 *   // Use <br> in content strings to produce multiple bullet points.
 */

import Stats from "stats.js";

export class DebugPanel extends HTMLElement {
  // ── URL params (read once at instantiation) ───────────────────────────────

  #p = new URLSearchParams(location.search);
  #spoutReceiverId = parseInt(this.#p.get("spoutReceiverId") ?? "0");
  #spoutInputName = this.#p.get("spoutInputName") ?? "";
  #spoutOutputName = this.#p.get("spoutOutputName") ?? "";

  // ── Instance state ────────────────────────────────────────────────────────

  #content = null; // text-sections div — innerHTML rebuilt on each redraw
  #statsSection = null; // persistent stats.js container — never in innerHTML
  #stats = null; // wrapped stats object exposed via getter
  #statsUsed = false; // true after first stats.begin() — shows perf section
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
        top: 10px; left: 10px;
        display: none;
        pointer-events: none;
        z-index: 9999;
      }
      :host([open]) { display: block; }
      .panel {
        background: rgba(0,0,0,0.85);
        color: rgba(255,255,255,0.82);
        font: 11px/1.7 monospace;
        padding: 10px 14px;
        border-radius: 4px;
        min-width: 300px;
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
    `;

    // Outer panel box — wraps both text sections and the persistent stats row
    const panelEl = document.createElement("div");
    panelEl.className = "panel";

    // Text sections — rebuilt via innerHTML on each redraw
    this.#content = document.createElement("div");
    panelEl.appendChild(this.#content);

    // Stats.js perf graphs — persistent DOM node, never wiped by innerHTML
    const rawStats = new Stats();
    Object.assign(rawStats.dom.style, {
      position: "static",
      display: "flex",
      cursor: "default",
      opacity: "1",
      zIndex: "auto",
    });
    this.#statsSection = document.createElement("div");
    this.#statsSection.className = "s";
    this.#statsSection.style.display = "none";
    const perfLbl = document.createElement("div");
    perfLbl.className = "lbl";
    perfLbl.textContent = "perf";
    this.#statsSection.appendChild(perfLbl);
    this.#statsSection.appendChild(rawStats.dom);
    panelEl.appendChild(this.#statsSection);

    shadow.appendChild(style);
    shadow.appendChild(panelEl);

    // Wrap begin/end so that first call flips #statsUsed and shows the section
    this.#stats = {
      begin: () => {
        this.#statsUsed = true;
        rawStats.begin();
      },
      end: () => rawStats.end(),
    };

    // Catch bubbled events from any <spout-receiver> on the page
    document.addEventListener("spout-connect", this.#onSpoutConnect);
    document.addEventListener("spout-frame", this.#onSpoutFrame);
    document.addEventListener("spout-disconnect", this.#onSpoutDisconnect);

    // Registered by bun's GlobalShortcut Ctrl+D / Ctrl+M via executeJavascript
    window.__ebPanelToggle = () => {
      const opening = !this.hasAttribute("open");
      this.toggleAttribute("open");
      if (opening) {
        this.#redraw();
        if (this.#onOpen) this.#onOpen();
      }
    };

    // Recompute fps counter once per second and redraw if open
    this.#intervalId = setInterval(() => {
      this.#spoutFPS = this.#spoutFrameCount - this.#spoutLastFPSCount;
      this.#spoutLastFPSCount = this.#spoutFrameCount;
      if (this.hasAttribute("open")) this.#redraw();
    }, 1000);
  }

  disconnectedCallback() {
    clearInterval(this.#intervalId);
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
   * Stats.js instance (begin/end wrapped). Call begin() + end() around each
   * render to populate the FPS / MS / MB graphs. The perf section is hidden
   * until the first begin() call, so views without a render loop stay clean.
   */
  get stats() {
    return this.#stats;
  }

  /**
   * Push view-specific section content. Keys and Spout sections are handled
   * internally; do not pass them here. Use <br> to emit multiple bullets.
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

  // ── Keys section with live toggle states ─────────────────────────────────

  #buildKeysHTML() {
    const st = window.__ebState || {};
    const aot = !!st.alwaysOnTop;
    const im = !!st.interactiveMode;
    const isSpoutMode = !!this.#spoutOutputName;

    const items = [
      `Ctrl+R — reload`,
      `Ctrl+F — always-on-top: ${DebugPanel.#badge(aot)}`,
      ...(!isSpoutMode ? [`Ctrl+M — interactive mode: ${DebugPanel.#badge(im)}`] : []),
      `Ctrl+D — toggle this panel`,
      `Esc — quit`,
    ];

    return "<ul>" + items.map((i) => `<li>${i}</li>`).join("") + "</ul>";
  }

  // ── Spout section ─────────────────────────────────────────────────────────

  #buildSpoutHTML() {
    const hasOutput = !!this.#spoutOutputName;
    const hasInput = !!(this.#spoutReceiverId || this.#spoutInputName);
    if (!hasOutput && !hasInput) return null;

    const items = [];

    if (hasOutput) {
      items.push(`output sender: <b>${this.#spoutOutputName}</b>`);
    }

    if (hasInput) {
      const name = this.#spoutInputName || "(unknown)";
      const id = this.#spoutReceiverId ? ` · receiver ${this.#spoutReceiverId}` : "";
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
    }

    return "<ul>" + items.map((i) => `<li>${i}</li>`).join("") + "</ul>";
  }

  // ── Full panel render ─────────────────────────────────────────────────────

  #redraw() {
    if (!this.#content || !this.hasAttribute("open")) return;

    const s = this.#viewSections;
    const spout = this.#buildSpoutHTML();

    let html =
      `<div class="s"><b>debug</b> <span style="opacity:0.4;font-weight:normal">— Ctrl+D to hide</span></div>` +
      `<div class="s"><div class="lbl">keys</div>${this.#buildKeysHTML()}</div>`;

    if (s.render != null) html += DebugPanel.#section("render", s.render);
    if (s.canvas != null) html += DebugPanel.#section("canvas", s.canvas);
    if (s.mouse != null) html += DebugPanel.#section("mouse", s.mouse);
    if (spout != null) html += `<div class="s"><div class="lbl">spout</div>${spout}</div>`;

    this.#content.innerHTML = html;

    // Show perf graphs only after the view has started calling begin()/end()
    this.#statsSection.style.display = this.#statsUsed ? "" : "none";
  }
}

if (!customElements.get("debug-panel")) {
  customElements.define("debug-panel", DebugPanel);
}
