/**
 * inject.js — auto-injects <debug-panel> into any page loaded in Chromeyumm.
 *
 * Bundled at build time into dist/debug-inject.js. The bun app reads this
 * file and evaluates it via executeJavascript on did-navigate, so every
 * contentUrl (bundled views, external dev servers, etc.) gets the debug
 * panel without needing to import the component manually.
 *
 * If the page already declares <debug-panel> in its HTML or imports the
 * module, the auto-injector detects it and skips creation.
 */
import "./debug-panel.js";

if (!document.querySelector("debug-panel")) {
  document.body.appendChild(document.createElement("debug-panel"));
}
