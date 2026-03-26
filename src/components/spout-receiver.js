/**
 * <spout-receiver> — self-contained web component for Spout input.
 *
 * Works in any HTML page loaded inside the Chromeyumm CEF browser.
 * No framework required. Pure vanilla JS — no build tools needed to read or
 * edit this file. Drop the compiled spout-receiver.js into any project.
 *
 * ─── USAGE ───────────────────────────────────────────────────────────────────
 *
 *   <script type="module" src="spout-receiver.js"></script>
 *
 *   <!-- Renders incoming frames directly (default) -->
 *   <spout-receiver style="width:100%;height:100%"></spout-receiver>
 *
 *   <!-- Events-only mode — pipe frames into Three.js, Canvas, etc. -->
 *   <spout-receiver render="false"></spout-receiver>
 *
 * ─── EVENTS ──────────────────────────────────────────────────────────────────
 *
 *   spout-connect     — first frame received; detail: { width, height }
 *   spout-disconnect  — feed stalled >1s after receiving at least one frame
 *   spout-frame       — every new frame; detail: { pixels: Uint8Array, width, height }
 *                       pixels: BGRA byte order, row-major, no padding
 *
 * ─── THREE.JS EXAMPLE ────────────────────────────────────────────────────────
 *
 *   const el = document.querySelector('spout-receiver');
 *   const tex = new THREE.DataTexture(null, 1, 1, THREE.RGBAFormat);
 *
 *   el.addEventListener('spout-frame', ({ detail: { pixels, width, height } }) => {
 *     tex.image = { data: pixels, width, height };
 *     tex.needsUpdate = true;
 *     // pixels are BGRA — apply .bgra swizzle in your Three.js shader
 *   });
 *
 * ─── HOW IT WORKS ────────────────────────────────────────────────────────────
 *
 *   Chromeyumm's CEF process helper injects one of two globals before
 *   any page script runs:
 *
 *     window.__spoutFrameBuffer  — ArrayBuffer (zero-copy shared memory,
 *                                  V8 sandbox disabled via --no-sandbox)
 *     window.__spoutGetFrame     — function(buf) → true|null (persistent-buffer
 *                                  copy, works with V8 sandbox enabled)
 *
 *   Shared memory layout (both paths):
 *     [0-3]   seq     Int32LE  — incremented by C++ after each frame write
 *     [4-7]   width   Uint32LE
 *     [8-11]  height  Uint32LE
 *     [12-15] reserved
 *     [16+]   BGRA pixels (up to 3840×2160×4 bytes)
 *
 *   The component polls each requestAnimationFrame, fires events on new frames,
 *   and (when render mode is on) uploads pixels to a WebGL texture with a
 *   BGRA→RGBA swizzle fragment shader so the image displays correctly.
 *   A checkered canvas overlay fades in when the feed is absent or stalled.
 */

export class SpoutReceiverElement extends HTMLElement {
  // ── Shaders ──────────────────────────────────────────────────────────────

  static #VERT = /* glsl */ `
		attribute vec2 a_pos;
		varying vec2 v_uv;
		void main() {
			v_uv = a_pos * 0.5 + 0.5;
			gl_Position = vec4(a_pos, 0.0, 1.0);
		}
	`;

  // Spout delivers BGRA. WebGL reads bytes into RGBA channels (byte[0]→R, etc.),
  // so the sampled .r is actually Blue and .b is Red. Swap them in the shader.
  static #FRAG = /* glsl */ `
		precision mediump float;
		uniform sampler2D u_frame;
		varying vec2 v_uv;
		void main() {
			vec4 c = texture2D(u_frame, vec2(v_uv.x, 1.0 - v_uv.y));
			gl_FragColor = vec4(c.b, c.g, c.r, c.a);
		}
	`;

  // ── Receiver factories ───────────────────────────────────────────────────

  static #createSharedMemoryReceiver(ab) {
    const seqView = new Int32Array(ab, 0, 1);
    const headerView = new Uint32Array(ab, 4, 2);
    const callbacks = [];
    let lastSeq = 0;

    return {
      poll() {
        const seq = seqView[0];
        if (seq === lastSeq || seq === 0) return;
        lastSeq = seq;
        const w = headerView[0],
          h = headerView[1];
        if (!w || !h) return;
        const pixels = new Uint8Array(ab, 16, w * h * 4);
        for (const cb of callbacks) cb(pixels, w, h);
      },
      onFrame(cb) {
        callbacks.push(cb);
      },
    };
  }

  static #createNativeFnReceiver(getFrame) {
    const kMaxBytes = 16 + 3840 * 2160 * 4;
    const persistentBuf = new ArrayBuffer(kMaxBytes);
    const hdr = new Uint32Array(persistentBuf, 0, 3); // [seq, width, height]
    const callbacks = [];

    return {
      poll() {
        if (!getFrame(persistentBuf)) return;
        const w = hdr[1],
          h = hdr[2];
        if (!w || !h) return;
        const pixels = new Uint8Array(persistentBuf, 16, w * h * 4);
        for (const cb of callbacks) cb(pixels, w, h);
      },
      onFrame(cb) {
        callbacks.push(cb);
      },
    };
  }

  static #createReceiver() {
    const ab = window.__spoutFrameBuffer;
    if (ab instanceof ArrayBuffer) return SpoutReceiverElement.#createSharedMemoryReceiver(ab);
    const fn = window.__spoutGetFrame;
    if (typeof fn === "function") return SpoutReceiverElement.#createNativeFnReceiver(fn);
    return null;
  }

  // ── WebGL pipeline ───────────────────────────────────────────────────────

  static #buildGLPipeline(canvas) {
    const gl = canvas.getContext("webgl");
    const prog = gl.createProgram();

    for (const [type, src] of [
      [gl.VERTEX_SHADER, SpoutReceiverElement.#VERT],
      [gl.FRAGMENT_SHADER, SpoutReceiverElement.#FRAG],
    ]) {
      const s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      gl.attachShader(prog, s);
    }
    gl.linkProgram(prog);
    gl.useProgram(prog);

    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const posLoc = gl.getAttribLocation(prog, "a_pos");
    gl.enableVertexAttribArray(posLoc);
    gl.vertexAttribPointer(posLoc, 2, gl.FLOAT, false, 0, 0);

    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.uniform1i(gl.getUniformLocation(prog, "u_frame"), 0);

    let texW = 0,
      texH = 0;

    return {
      upload(pixels, w, h) {
        if (w !== texW || h !== texH) {
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
          texW = w;
          texH = h;
        } else {
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        }
      },
      draw() {
        gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      },
    };
  }

  // ── Checker canvas ───────────────────────────────────────────────────────

  static #drawChecker(canvas) {
    const ctx = canvas.getContext("2d");
    const size = 48;
    for (let y = 0; y < canvas.height; y += size) {
      for (let x = 0; x < canvas.width; x += size) {
        ctx.fillStyle = (x / size + y / size) % 2 === 0 ? "#555" : "#333";
        ctx.fillRect(x, y, size, size);
      }
    }
  }

  // ── Instance ─────────────────────────────────────────────────────────────

  #receiver = null;
  #gl = null;
  #checker = null; // 2D canvas overlay shown when disconnected
  #rafId = 0;
  #hasFrame = false;
  #disconnected = true; // starts disconnected until first frame arrives
  #lastFrameTime = 0;

  /** The WebGL canvas used for rendering — exposed for subclass use (e.g. captureStream). */
  _glCanvas = null;

  static get observedAttributes() {
    return ["render"];
  }

  connectedCallback() {
    const shadow = this.attachShadow({ mode: "open" });

    const style = document.createElement("style");
    style.textContent = /* css */ `
			:host { 
				display: block; 
				overflow: hidden; 
				background: #000; 
				position: relative;
			}
			canvas { 
				display: block; width: 100%; height: 100%;
			}
			.checker {
				position: absolute; inset: 0;
				pointer-events: none;
				opacity: 1;
				transition: opacity 0.4s ease;
			}
			.checker.hidden { 
				opacity: 0; 
			}
			.msg { 
				position: absolute; inset: 0; display: flex; align-items: center;
				justify-content: center; color: rgba(255,255,255,0.4);
				font: 13px/1.4 monospace; text-align: center; padding: 16px;
			}
		`;
    shadow.appendChild(style);

    const shouldRender = this.getAttribute("render") !== "false";

    if (shouldRender) {
      // WebGL canvas — Spout frames rendered here
      const glCanvas = document.createElement("canvas");
      glCanvas.width = this.clientWidth || window.innerWidth;
      glCanvas.height = this.clientHeight || window.innerHeight;
      shadow.appendChild(glCanvas);
      this._glCanvas = glCanvas;
      this.#gl = SpoutReceiverElement.#buildGLPipeline(glCanvas);

      // Checker canvas — drawn once, overlaid on top, fades out when connected
      const checker = document.createElement("canvas");
      checker.className = "checker";
      checker.width = glCanvas.width;
      checker.height = glCanvas.height;
      SpoutReceiverElement.#drawChecker(checker);
      shadow.appendChild(checker);
      this.#checker = checker;
    }

    this.#receiver = SpoutReceiverElement.#createReceiver();

    if (!this.#receiver) {
      if (shouldRender) {
        const msg = document.createElement("div");
        msg.className = "msg";
        msg.textContent =
          "No Spout input — set spoutInput in display-config.json and ensure spoutReceiverId is in the page URL";
        shadow.appendChild(msg);
      }
      console.warn("[spout-receiver] Neither __spoutFrameBuffer nor __spoutGetFrame found.");
      return;
    }

    this.#receiver.onFrame((pixels, w, h) => {
      if (!this.#hasFrame) {
        this.#hasFrame = true;
        this.dispatchEvent(
          new CustomEvent("spout-connect", {
            bubbles: true,
            composed: true,
            detail: { width: w, height: h },
          }),
        );
      }
      this.#lastFrameTime = performance.now();

      // Keep the GL canvas sized to the actual Spout input dimensions.
      if (this._glCanvas && (this._glCanvas.width !== w || this._glCanvas.height !== h)) {
        this._glCanvas.width = w;
        this._glCanvas.height = h;
      }

      // Reconnect: hide checker if it was showing
      if (this.#disconnected) {
        this.#disconnected = false;
        this.#checker?.classList.add("hidden");
      }

      this.dispatchEvent(
        new CustomEvent("spout-frame", {
          bubbles: true,
          composed: true,
          detail: { pixels, width: w, height: h },
        }),
      );
      this.#gl?.upload(pixels, w, h);
    });

    const loop = () => {
      this.#rafId = requestAnimationFrame(loop);
      this.#receiver?.poll();

      // Show checker when feed has stalled for more than 1 second
      if (!this.#disconnected && performance.now() - this.#lastFrameTime > 1000) {
        this.#disconnected = true;
        this.#checker?.classList.remove("hidden");
        this.dispatchEvent(
          new CustomEvent("spout-disconnect", {
            bubbles: true,
            composed: true,
          }),
        );
      }

      if (this.#hasFrame) {
        this.#gl?.draw();
        this._afterDraw();
      }
    };
    loop();
  }

  /**
   * Called after each WebGL draw in the rAF loop.
   * Override in subclasses to hook into the render cycle (e.g. captureStream).
   */
  _afterDraw() {}

  disconnectedCallback() {
    cancelAnimationFrame(this.#rafId);
  }
}

// Guard against double-registration (e.g. when spout-video.js bundles this file inline)
if (!customElements.get("spout-receiver")) {
  customElements.define("spout-receiver", SpoutReceiverElement);
}
