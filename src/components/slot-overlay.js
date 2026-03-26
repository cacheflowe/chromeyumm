/**
 * slot-overlay.js — slot boundary overlay web component.
 *
 * Renders a coordinate grid and per-slot boundary boxes over the full virtual
 * canvas. Works in any view — drop in alongside layout-params.js.
 *
 * ─── USAGE ────────────────────────────────────────────────────────────────────
 *
 *   Place in HTML:
 *     <slot-overlay></slot-overlay>
 *
 *   Import from your script to register the element:
 *     import "../../components/slot-overlay.js";
 *
 *   Toggle via window.__ebDebugToggle() — wired to Ctrl+D by bun.
 *
 * ─── URL PARAMETERS ───────────────────────────────────────────────────────────
 *
 *   totalWidth, totalHeight  — virtual canvas dimensions (default: 1920×1080)
 *   slots                    — JSON array of slot objects passed by bun:
 *                              [{ slot, x, y, w, h, sim }, ...]
 *
 *   The component renders nothing visible if the slots param is absent, but
 *   still registers window.__ebDebugToggle so the Ctrl+D shortcut is safe.
 */

const SLOT_COLORS = ["#00ffff", "#ffff00", "#ff00ff", "#00ff88", "#ff8800"];
const GRID_PX = 100;

export class SlotOverlay extends HTMLElement {
  connectedCallback() {
    const p = new URLSearchParams(location.search);
    const totalWidth = parseInt(p.get("totalWidth") ?? "1920");
    const totalHeight = parseInt(p.get("totalHeight") ?? "1080");
    const slotsParam = p.get("slots");
    const slots = slotsParam ? JSON.parse(slotsParam) : [];

    // Always register the toggle so Ctrl+D is safe even with no slot data.
    window.__ebDebugToggle = () => this.toggleAttribute("visible");

    const shadow = this.attachShadow({ mode: "open" });

    const style = document.createElement("style");
    style.textContent = `
      :host {
        position: fixed;
        top: 0; left: 0;
        width: 100%; height: 100%;
        display: none;
        pointer-events: none;
        z-index: 2147483647;
        overflow: hidden;
        font-family: monospace;
      }
      :host([visible]) { display: block; }
    `;
    shadow.appendChild(style);

    if (!slots.length) return;

    const vpW = window.innerWidth;
    const vpH = window.innerHeight;
    const sx = vpW / totalWidth;
    const sy = vpH / totalHeight;

    const cx = (lx) => (lx * sx).toFixed(2) + "px";
    const cy = (ly) => (ly * sy).toFixed(2) + "px";

    // ── Coordinate grid ───────────────────────────────────────────────────────

    const grid = document.createElement("div");
    grid.style.cssText = [
      "position:absolute",
      "inset:0",
      "background-image:" +
        "linear-gradient(to right,  rgba(255,255,255,0.10) 1px, transparent 1px)," +
        "linear-gradient(to bottom, rgba(255,255,255,0.10) 1px, transparent 1px)",
      "background-size:" + (GRID_PX * sx).toFixed(2) + "px " + (GRID_PX * sy).toFixed(2) + "px",
    ].join(";");
    shadow.appendChild(grid);

    // ── X-axis coordinate labels ──────────────────────────────────────────────

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
      shadow.appendChild(lbl);
    }

    // ── Y-axis coordinate labels ──────────────────────────────────────────────

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
      shadow.appendChild(lbl);
    }

    // ── Per-slot boundary boxes + labels ─────────────────────────────────────

    slots.forEach((slot, i) => {
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
      shadow.appendChild(box);

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
      shadow.appendChild(info);
    });
  }
}

if (!customElements.get("slot-overlay")) {
  customElements.define("slot-overlay", SlotOverlay);
}
