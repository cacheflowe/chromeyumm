// @types/p5 (still v1-targeted) doesn't yet expose:
// - the single-argument filter-shader APIs (createFilterShader / filter(shader)) added in p5 1.4+.
// - fontWidth(), p5 v2's new name for the old (advance-width) textWidth() behavior —
//   see the comment above kernedWidth() in main.ts for why this project needs it specifically.
import "p5";

declare module "p5" {
  interface p5InstanceExtensions {
    createFilterShader(fragSrc: string): p5.Shader;
    filter(shader: p5.Shader): void;
    fontWidth(text: string): number;
  }
}
