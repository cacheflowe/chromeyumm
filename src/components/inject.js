/**
 * inject.js — auto-injects debug-panel and slot-overlay into any page.
 *
 * Bundled at build time into dist/debug-inject.js. The bun app reads this
 * file and evaluates it via executeJavascript on dom-ready, so every
 * contentUrl (bundled views, external dev servers, etc.) gets the debug
 * panel without needing to import the components manually.
 */
import "./debug-panel.js";
import "./slot-overlay.js";

if (!document.querySelector("debug-panel")) {
  document.body.appendChild(document.createElement("debug-panel"));
}
if (!document.querySelector("slot-overlay")) {
  document.body.appendChild(document.createElement("slot-overlay"));
}
