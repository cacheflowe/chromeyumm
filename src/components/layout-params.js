/**
 * layout-params.js — parse standard Chromeyumm URL parameters.
 *
 * Works in any page — no build tools, no framework required.
 * Drop into an external dev site to receive the same layout parameters
 * that Chromeyumm passes to its built-in views.
 *
 * URL parameters:
 *   slot            zero-based index of this window in the layout  (default: 0)
 *   totalSlots      total number of display slots                  (default: 1)
 *   totalWidth      full virtual canvas width in px                (default: 1920)
 *   totalHeight     full virtual canvas height in px               (default: 1080)
 *   sourceX         left edge of this slot's region                (default: 0)
 *   sourceY         top edge of this slot's region                 (default: 0)
 *   sourceWidth     width of this slot's region                    (default: totalWidth)
 *   sourceHeight    height of this slot's region                   (default: totalHeight)
 *   simulated       "true" when running in simulation mode         (default: false)
 *   spoutReceiverId numeric ID for the Spout input receiver        (default: 0 = none)
 */

/**
 * @returns {{
 *   slot: number,
 *   totalSlots: number,
 *   totalWidth: number,
 *   totalHeight: number,
 *   sourceX: number,
 *   sourceY: number,
 *   sourceWidth: number,
 *   sourceHeight: number,
 *   simulated: boolean,
 *   spoutReceiverId: number,
 * }}
 */
export function parseLayoutParams() {
  const p = new URLSearchParams(location.search);
  const totalWidth = parseInt(p.get("totalWidth") ?? "1920");
  const totalHeight = parseInt(p.get("totalHeight") ?? "1080");
  return {
    slot: parseInt(p.get("slot") ?? "0"),
    totalSlots: parseInt(p.get("totalSlots") ?? "1"),
    totalWidth,
    totalHeight,
    sourceX: parseInt(p.get("sourceX") ?? "0"),
    sourceY: parseInt(p.get("sourceY") ?? "0"),
    sourceWidth: parseInt(p.get("sourceWidth") ?? String(totalWidth)),
    sourceHeight: parseInt(p.get("sourceHeight") ?? String(totalHeight)),
    simulated: p.get("simulated") === "true",
    spoutReceiverId: parseInt(p.get("spoutReceiverId") ?? "0"),
  };
}
