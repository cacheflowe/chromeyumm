import * as THREE from "three";

/**
 * Renders a fullscreen-quad shader into an offscreen FBO (render target).
 * Useful for generating procedural textures (gradients, noise, etc.)
 * that can be used as scene backgrounds or material maps.
 */
export class ThreeSceneFBO {
  private renderTarget: THREE.WebGLRenderTarget;
  private scene: THREE.Scene;
  private camera: THREE.OrthographicCamera;
  private quad: THREE.Mesh;
  private debugRenderer: THREE.WebGLRenderer | null = null;

  /** A simple passthrough vertex shader for use with RawShaderMaterial. */
  static defaultRawVertShader = /* glsl */ `
    precision highp float;
    attribute vec3 position;
    attribute vec2 uv;
    varying vec2 vUv;
    varying vec3 vPos;
    void main() {
      vUv = uv;
      vPos = position;
      gl_Position = vec4(position, 1.0);
    }
  `;

  constructor(
    width: number,
    height: number,
    private clearColor: number = 0x000000,
  ) {
    this.renderTarget = new THREE.WebGLRenderTarget(width, height, {
      minFilter: THREE.LinearFilter,
      magFilter: THREE.LinearFilter,
      format: THREE.RGBAFormat,
    });

    this.scene = new THREE.Scene();
    this.camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);

    // Fullscreen quad — PlaneGeometry(2,2) spans NDC [-1,1]
    const geo = new THREE.PlaneGeometry(2, 2);
    this.quad = new THREE.Mesh(geo);
    this.scene.add(this.quad);
  }

  /** Set the material rendered onto the fullscreen quad. */
  setMaterial(mat: THREE.Material) {
    this.quad.material = mat;
  }

  /** Returns the FBO texture (can be assigned to scene.background, uniforms, etc.). */
  getTexture(): THREE.Texture {
    return this.renderTarget.texture;
  }

  /**
   * Creates a small debug canvas that mirrors the FBO contents each frame.
   * Append the returned canvas to the DOM yourself.
   */
  addDebugCanvas(): HTMLCanvasElement {
    this.debugRenderer = new THREE.WebGLRenderer({ antialias: false });
    this.debugRenderer.setSize(this.renderTarget.width, this.renderTarget.height, false);
    return this.debugRenderer.domElement;
  }

  /** Render the shader into the offscreen FBO, optionally updating the debug canvas. */
  render(mainRenderer: THREE.WebGLRenderer) {
    // Save current state
    const prevTarget = mainRenderer.getRenderTarget();
    const prevClearColor = new THREE.Color();
    mainRenderer.getClearColor(prevClearColor);
    const prevClearAlpha = mainRenderer.getClearAlpha();

    // Render to FBO
    mainRenderer.setRenderTarget(this.renderTarget);
    mainRenderer.setClearColor(this.clearColor, 1);
    mainRenderer.clear();
    mainRenderer.render(this.scene, this.camera);

    // Restore
    mainRenderer.setRenderTarget(prevTarget);
    mainRenderer.setClearColor(prevClearColor, prevClearAlpha);

    // Update debug canvas if present
    if (this.debugRenderer) {
      this.debugRenderer.render(this.scene, this.camera);
    }
  }

  dispose() {
    this.renderTarget.dispose();
    this.debugRenderer?.dispose();
  }
}
