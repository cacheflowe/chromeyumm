// Ported as-is from the client's React/p5 ticker app — converts anti-aliased
// text edges into hard on/off alpha, which reads much better on physical LED
// panels than soft-edged text.
const alphaThreshold = /* glsl */ `
  precision mediump float;

  varying vec2 vTexCoord;
  uniform sampler2D tex0;
  uniform float uThreshold;
  uniform vec3 uTextColor;

  void main() {
    vec2 uv = vTexCoord.xy;
    vec4 texColor = texture2D(tex0, uv);
    float alpha = (texColor.a > uThreshold) ? 1.0 : 0.0;
    // p5's WebGL filter compositing blends with premultiplied alpha, so
    // non-premultiplied output here would leak uTextColor into transparent
    // (alpha 0) background pixels.
    gl_FragColor = vec4(uTextColor * alpha, alpha);
  }
`;

export default alphaThreshold;
