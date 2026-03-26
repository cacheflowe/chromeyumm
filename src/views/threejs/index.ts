import * as THREE from "three";
import { parseLayoutParams } from "../../components/layout-params.js";
import "../../components/debug-panel.js";
import "../../components/slot-overlay.js";
import { ThreeSceneFBO } from "./ThreeSceneFBO.js";
// Type-only interface for the <debug-panel> web component
interface DebugPanelEl extends HTMLElement {
  update(s: { render?: string | null; canvas?: string | null; mouse?: string | null }): void;
  onOpen: (() => void) | null;
  readonly visible: boolean;
  readonly stats: { begin(): void; end(): number };
}
import "../../components/spout-receiver.js";

// ---------------------------------------------------------------------------
// Layout from URL params
// ---------------------------------------------------------------------------

const {
  slot,
  totalSlots,
  totalWidth,
  totalHeight,
  sourceX,
  sourceY,
  sourceWidth,
  sourceHeight,
  simulated,
  spoutReceiverId,
} = parseLayoutParams();

const vpW = window.innerWidth;
const vpH = window.innerHeight;
const dpr = window.devicePixelRatio || 1;

// ---------------------------------------------------------------------------
// DOM
// ---------------------------------------------------------------------------

const canvas = document.getElementById("canvas") as HTMLCanvasElement;
const waitEl = document.getElementById("waiting") as HTMLDivElement;

// ---------------------------------------------------------------------------
// Three.js renderer
//
// canvas.width = sourceWidth (= physical pixels configured for this window).
// canvas.style.width = vpW (CSS pixels; browser scales buffer to display size).
// renderer.setPixelRatio(1) since we manage the buffer size ourselves.
// ---------------------------------------------------------------------------

const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
renderer.setPixelRatio(1);
renderer.setSize(sourceWidth, sourceHeight, false); // false = don't set CSS style
canvas.style.width = vpW + "px";
canvas.style.height = vpH + "px";
renderer.shadowMap.enabled = false;
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 2.2;

// ---------------------------------------------------------------------------
// Spout input background (optional — activated by ?spoutReceiverId=N)
// Reads from window.__spoutFrameBuffer (Win32 named shared memory).
// Renders an orthographic fullscreen quad behind the 3D scene.
// Pixel format from Spout is BGRA; the fragment shader swizzles to RGBA.
// ---------------------------------------------------------------------------

let bgScene: THREE.Scene | null = null;
let bgCamera: THREE.OrthographicCamera | null = null;
let bgMat: THREE.ShaderMaterial | null = null;
let spoutTex: THREE.DataTexture | null = null;

if (spoutReceiverId) {
  // Use <spout-receiver render="false"> for event-based frame access.
  // The element handles shared-memory polling internally; we update a
  // DataTexture from spout-frame events instead of calling poll() each rAF.
  const receiverEl = document.createElement("spout-receiver") as HTMLElement;
  receiverEl.setAttribute("render", "false");
  receiverEl.style.display = "none";
  document.body.appendChild(receiverEl);

  bgScene = new THREE.Scene();
  bgCamera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
  bgMat = new THREE.ShaderMaterial({
    uniforms: { uTex: { value: null } },
    vertexShader: ["varying vec2 vUv;", "void main() { vUv = uv; gl_Position = vec4(position, 1.0); }"].join("\n"),
    fragmentShader: [
      "uniform sampler2D uTex;",
      "varying vec2 vUv;",
      // BGRA data stored in RGBA texture → swizzle back to correct colours
      "void main() { gl_FragColor = texture2D(uTex, vUv).bgra; }",
    ].join("\n"),
    depthTest: false,
    depthWrite: false,
  });
  bgScene.add(new THREE.Mesh(new THREE.PlaneGeometry(2, 2), bgMat));

  let texW = 0,
    texH = 0;
  receiverEl.addEventListener("spout-frame", (e) => {
    const { pixels, width: w, height: h } = (e as CustomEvent).detail;
    if (!spoutTex || texW !== w || texH !== h) {
      spoutTex?.dispose();
      spoutTex = new THREE.DataTexture(pixels, w, h, THREE.RGBAFormat, THREE.UnsignedByteType);
      spoutTex.flipY = false;
      texW = w;
      texH = h;
    } else {
      (spoutTex.image as any).data = pixels;
    }
    spoutTex.needsUpdate = true;
  });
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

const scene = new THREE.Scene();
// Only use the solid background colour when Spout input is not providing it.
if (!spoutReceiverId) scene.background = new THREE.Color(0x0d0830);
scene.fog = new THREE.FogExp2(0x0d0830, 0.012);

// ---------------------------------------------------------------------------
// Procedural background shader (fallback when no Spout input)
// ---------------------------------------------------------------------------

let threeFBO: ThreeSceneFBO | null = null;
let gradientMaterial: THREE.RawShaderMaterial | null = null;
let bgTexture: THREE.Texture | null = null;

function buildBackgroundFbo() {
  threeFBO = new ThreeSceneFBO(512, 512, 0x00ffff);
  const debugFboCanvas = threeFBO.addDebugCanvas();

  // add debug renderer to DOM
  const debugContainer = document.querySelector("debug-panel") as HTMLElement;
  if (debugContainer) {
    debugContainer.appendChild(debugFboCanvas);
  } else {
    document.body.appendChild(debugFboCanvas);
  }
  debugFboCanvas.style.setProperty("position", "fixed");
  debugFboCanvas.style.setProperty("bottom", "4px");
  debugFboCanvas.style.setProperty("right", "4px");
  debugFboCanvas.style.setProperty("width", "128px");
  debugFboCanvas.style.setProperty("height", "128px");
  debugFboCanvas.style.setProperty("border", "2px solid #000");
  debugFboCanvas.style.setProperty("box-sizing", "border-box");
  debugFboCanvas.style.setProperty("z-index", "9999");
  debugFboCanvas.style.setProperty("pointer-events", "none");

  // create shader material
  gradientMaterial = new THREE.RawShaderMaterial({
    uniforms: {
      time: { value: 0.0 },
    },
    vertexShader: ThreeSceneFBO.defaultRawVertShader,
    fragmentShader: /* glsl */ `
      precision highp float;

      uniform float time;
      varying vec2 vUv;
      varying vec3 vPos;

      void main() {
        // radial gradient
        float gradientProgress = -time * 15. + length(vUv - 0.5) * 30.;
        gl_FragColor = vec4(
          0.0 + 0.15 * sin(gradientProgress * 5.),
          0.3 + 0.15 * sin(gradientProgress * 4.),
          0.2 + 0.15 * sin(gradientProgress * 3.),
          1.
        );
      }
    `,
  });
  threeFBO.setMaterial(gradientMaterial);

  // set texture on bg
  bgTexture = threeFBO.getTexture();
  scene.background = bgTexture;
  updateBgAspect();
}

function updateBgAspect() {
  if (!bgTexture) return;
  const img = bgTexture.image as { width: number; height: number } | null;
  const imageAspect = img ? img.width / img.height : 1;
  const aspect = imageAspect / (totalWidth / totalHeight);

  bgTexture.offset.x = aspect > 1 ? (1 - 1 / aspect) / 2 : 0;
  bgTexture.repeat.x = aspect > 1 ? 1 / aspect : 1;

  bgTexture.offset.y = aspect > 1 ? 0 : (1 - aspect) / 2;
  bgTexture.repeat.y = aspect > 1 ? 1 : aspect;
}

function updateShaderBg() {
  if (!threeFBO || !gradientMaterial) return;

  // update & render shader
  const time = performance.now() * 0.0001;
  gradientMaterial.uniforms["time"]!.value = time;
  threeFBO.render(renderer);
  updateBgAspect();
}

if (!spoutReceiverId) {
  buildBackgroundFbo();
}

// ---------------------------------------------------------------------------
// Camera
//
// Aspect ratio based on FULL virtual canvas — Three.js computes the correct
// sub-frustum via setViewOffset.
// ---------------------------------------------------------------------------

const camera = new THREE.PerspectiveCamera(55, totalWidth / totalHeight, 0.1, 200);
camera.position.set(0, 4, 22);
camera.lookAt(0, 0, 0);

// This is the key call: tell Three.js this renderer shows only a sub-region
// of the full totalWidth × totalHeight virtual canvas.
camera.setViewOffset(totalWidth, totalHeight, sourceX, sourceY, sourceWidth, sourceHeight);

// ---------------------------------------------------------------------------
// Geometry & materials
// ---------------------------------------------------------------------------

// 3 000 instanced metallic icosahedra
const COUNT = 3000;
const geoIco = new THREE.IcosahedronGeometry(0.28, 1);
const matMetal = new THREE.MeshStandardMaterial({
  color: 0xffffff,
  roughness: 0.05,
  metalness: 0.9,
  emissive: new THREE.Color(0x224466),
  emissiveIntensity: 0.4,
});
const instancedMesh = new THREE.InstancedMesh(geoIco, matMetal, COUNT);
instancedMesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
scene.add(instancedMesh);

// Random base positions: spread in a sphere shell
const basePos: Float32Array = new Float32Array(COUNT * 4); // x, y, z, phase
for (let i = 0; i < COUNT; i++) {
  const r = 4 + Math.random() * 12;
  const theta = Math.random() * Math.PI * 2;
  const phi = Math.acos(2 * Math.random() - 1);
  basePos[i * 4 + 0] = r * Math.sin(phi) * Math.cos(theta);
  basePos[i * 4 + 1] = r * Math.sin(phi) * Math.sin(theta);
  basePos[i * 4 + 2] = r * Math.cos(phi);
  basePos[i * 4 + 3] = Math.random() * Math.PI * 2; // phase offset
}

// Large ground plane
// const ground = new THREE.Mesh(
//   new THREE.PlaneGeometry(100, 100),
//   new THREE.MeshStandardMaterial({ color: 0x1a0a2e, roughness: 0.7, metalness: 0.3 }),
// );
// ground.rotation.x = -Math.PI / 2;
// ground.position.y = -10;
// scene.add(ground);

// Large slow-spinning rings for depth
const ringColors = [0xff2255, 0x2255ff, 0x00ffaa];
for (let i = 0; i < 3; i++) {
  const ring = new THREE.Mesh(
    new THREE.TorusGeometry(6 + i * 3, 0.08, 8, 80),
    new THREE.MeshStandardMaterial({
      color: ringColors[i],
      roughness: 0.2,
      metalness: 0.8,
      emissive: new THREE.Color(ringColors[i]!),
      emissiveIntensity: 0.6,
    }),
  );
  ring.rotation.x = (Math.PI / 3) * i;
  ring.rotation.y = (Math.PI / 5) * i;
  scene.add(ring);
}

// ---------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------

scene.add(new THREE.AmbientLight(0x8844ff, 2.0));

const lightDefs = [
  { color: 0xff2255, intensity: 20, radius: 12, speed: 0.28, phase: 0 },
  { color: 0x2255ff, intensity: 20, radius: 10, speed: 0.22, phase: Math.PI / 2 },
  { color: 0x00ffaa, intensity: 14, radius: 14, speed: 0.18, phase: Math.PI },
  { color: 0xff8800, intensity: 12, radius: 8, speed: 0.35, phase: Math.PI * 1.5 },
];

const lights = lightDefs.map(({ color, intensity, radius, speed, phase }) => {
  const light = new THREE.PointLight(color, intensity, radius * 2.5);
  scene.add(light);

  // Visible sphere at light position
  const sphere = new THREE.Mesh(new THREE.SphereGeometry(0.15, 8, 8), new THREE.MeshBasicMaterial({ color }));
  scene.add(sphere);

  return { light, sphere, radius, speed, phase };
});

// ---------------------------------------------------------------------------
// Debug panel — toggled by Ctrl+D (GlobalShortcut → executeJavascript from bun)
// ---------------------------------------------------------------------------

const panel = document.querySelector("debug-panel") as unknown as DebugPanelEl;
const stats = panel.stats;

// Timer for the once-per-second panel text update
let lastPanelUpdate = performance.now();

function updatePanel() {
  const info = renderer.info;
  const isSubRegion = totalSlots > 1 || sourceX !== 0 || sourceY !== 0;
  const mStr = lastMouseX === -9999 ? "no events yet" : `${lastMouseX}, ${lastMouseY}`;

  let canvasStr = `${sourceWidth}×${sourceHeight} px · css ${vpW}×${vpH} · dpr ${dpr.toFixed(2)}`;
  if (isSubRegion) {
    canvasStr +=
      `<br>slot ${slot}/${totalSlots - 1} · src (${sourceX}, ${sourceY} ${sourceWidth}×${sourceHeight})` +
      (simulated ? " · simulated" : "") +
      `<br>virtual ${totalWidth}×${totalHeight}`;
  }
  const sx = vpW / totalWidth;
  const sy = vpH / totalHeight;
  if (Math.abs(sx - 1) > 0.01 || Math.abs(sy - 1) > 0.01) {
    canvasStr += `<br>scale ${sx.toFixed(3)}×${sy.toFixed(3)}`;
  }

  panel.update({
    render: `${info.render.calls} draw calls · ${info.render.triangles.toLocaleString()} tris`,
    canvas: canvasStr,
    mouse: mStr,
  });

  renderer.info.reset();
}

// Refresh immediately on open so content isn't stale
panel.onOpen = updatePanel;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const dummy = new THREE.Object3D();

// ---------------------------------------------------------------------------
// Animation loop
// ---------------------------------------------------------------------------

waitEl.style.display = "none";

// Mouse-driven camera tilt — updated by mousemove listener below.
// If events reach the browser, moving the mouse noticeably tilts the scene.
let mouseNX = 0; // -1 (left) … +1 (right)
let mouseNY = 0; // -1 (top)  … +1 (bottom)
let mouseSmX = 0; // smoothed for rendering
let mouseSmY = 0;
let lastMouseX = -9999;
let lastMouseY = -9999;

document.addEventListener("mousemove", (e) => {
  lastMouseX = e.clientX;
  lastMouseY = e.clientY;
  mouseNX = (e.clientX / window.innerWidth) * 2 - 1;
  mouseNY = (e.clientY / window.innerHeight) * 2 - 1;
});

function animate() {
  requestAnimationFrame(animate);
  stats.begin();

  // Use wall-clock time so all windows stay frame-for-frame in sync regardless
  // of when they loaded or reloaded.
  const t = Date.now() * 0.001;

  // --- Update instanced mesh ---
  for (let i = 0; i < COUNT; i++) {
    const x = basePos[i * 4 + 0]!;
    const y = basePos[i * 4 + 1]!;
    const z = basePos[i * 4 + 2]!;
    const phase = basePos[i * 4 + 3]!;

    dummy.position.set(
      x + Math.sin(t * 0.3 + phase) * 0.4,
      y + Math.sin(t * 0.5 + phase * 1.3) * 0.6,
      z + Math.cos(t * 0.4 + phase * 0.7) * 0.4,
    );
    dummy.rotation.set(t * 0.4 + phase, t * 0.6 + phase, t * 0.2);
    dummy.updateMatrix();
    instancedMesh.setMatrixAt(i, dummy.matrix);
  }
  instancedMesh.instanceMatrix.needsUpdate = true;

  // --- Move lights ---
  lights.forEach(({ light, sphere, radius, speed, phase }) => {
    const angle = t * speed + phase;
    const vy = Math.sin(t * 0.15 + phase) * 4;
    const pos = new THREE.Vector3(radius * Math.cos(angle), vy, radius * Math.sin(angle));
    light.position.copy(pos);
    sphere.position.copy(pos);
  });

  // --- Rotate scene (auto + mouse influence) ---
  mouseSmX += (mouseNX - mouseSmX) * 0.05;
  mouseSmY += (mouseNY - mouseSmY) * 0.05;
  scene.rotation.y = t * 0.04 + mouseSmX * 0.6;
  scene.rotation.x = mouseSmY * 0.3;

  // --- Update procedural background shader (when no Spout) ---
  if (!spoutReceiverId) {
    updateShaderBg();
  }

  // --- Render: background (Spout) → depth clear → 3D scene ---
  if (bgScene && bgCamera && bgMat && spoutTex) {
    bgMat.uniforms.uTex.value = spoutTex;
    renderer.autoClear = true;
    renderer.render(bgScene, bgCamera);
    renderer.clearDepth(); // clear depth so 3D draws in front
    renderer.autoClear = false; // don't wipe the background we just drew
    renderer.render(scene, camera);
    renderer.autoClear = true;
  } else {
    renderer.render(scene, camera);
  }

  // stats.end() returns performance.now() — reuse for the 1s panel text update
  const now = stats.end();
  if (now - lastPanelUpdate >= 1000) {
    lastPanelUpdate = now;
    if (panel.visible) updatePanel();
    else renderer.info.reset(); // reset even when hidden to avoid count overflow
  }
}

// ---------------------------------------------------------------------------
// Cursor idle hiding
// ---------------------------------------------------------------------------

let cursorTimer: ReturnType<typeof setTimeout> | null = null;

function resetCursorTimer() {
  document.body.style.cursor = "default";
  if (cursorTimer) clearTimeout(cursorTimer);
  cursorTimer = setTimeout(() => {
    document.body.style.cursor = "none";
  }, 3000);
}

document.addEventListener("mousemove", resetCursorTimer);
document.body.style.cursor = "none";

requestAnimationFrame(animate);
