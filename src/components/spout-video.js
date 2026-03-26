/**
 * <spout-video> — Spout input → native <video> element via canvas.captureStream().
 *
 * Extends <spout-receiver>: inherits all frame polling, WebGL BGRA→RGBA rendering,
 * checker overlay, and spout-* events. Adds a <video> element that displays the
 * same rendered frames via MediaStream, and hides the underlying GL canvas.
 *
 * Depends on spout-receiver.js (must be in the same directory).
 * Importing this file registers both <spout-receiver> and <spout-video>.
 *
 * ─── USAGE ───────────────────────────────────────────────────────────────────
 *
 *   <script type="module" src="spout-video.js"></script>
 *
 *   <spout-video style="width:640px;height:360px"></spout-video>
 *
 * ─── EVENTS ──────────────────────────────────────────────────────────────────
 *
 *   spout-connect, spout-disconnect, spout-frame  (inherited from <spout-receiver>)
 *
 * ─── VIDEO ELEMENT ACCESS ────────────────────────────────────────────────────
 *
 *   el.video  — the inner HTMLVideoElement
 *
 *   Useful for PiP, recording, or piping the MediaStream elsewhere:
 *     const stream = el.video.srcObject;
 */

import { SpoutReceiverElement } from "./spout-receiver.js";

class SpoutVideoElement extends SpoutReceiverElement {
  #track = null;

  /** The inner HTMLVideoElement — exposed for external access. */
  video = null;

  connectedCallback() {
    super.connectedCallback(); // sets up shadow DOM, GL canvas, checker, rAF loop

    // Hide the inherited GL canvas (still renders off-screen for captureStream)
    // and style the <video> we're about to add.
    const style = document.createElement("style");
    style.textContent = `
			canvas { display: none !important; }
			video { display: block; width: 100%; height: 100%; object-fit: fill; }
		`;
    this.shadowRoot.appendChild(style);

    // captureStream(0) = manual capture; requestFrame() called after each draw.
    const stream = this._glCanvas?.captureStream(0) ?? null;
    this.#track = stream?.getVideoTracks()[0] ?? null;

    const video = document.createElement("video");
    video.setAttribute("part", "video"); // allows external CSS: spout-video::part(video) { ... }
    video.srcObject = stream;
    video.autoplay = true;
    video.muted = true;
    video.playsInline = true;
    this.video = video;

    // Insert video before the checker overlay so the checker stays on top
    const checker = this.shadowRoot.querySelector(".checker");
    this.shadowRoot.insertBefore(video, checker ?? null);
  }

  // Called by the parent's rAF loop after each WebGL draw.
  _afterDraw() {
    this.#track?.requestFrame();
  }
}

if (!customElements.get("spout-video")) {
  customElements.define("spout-video", SpoutVideoElement);
}
