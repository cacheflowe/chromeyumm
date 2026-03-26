import { parseLayoutParams } from "../../components/layout-params.js";
import "../../components/debug-panel.js";
import "../../components/spout-video.js";

const { totalWidth, totalHeight } = parseLayoutParams();

const panel = document.querySelector("debug-panel") as HTMLElement & {
  update(s: { canvas?: string }): void;
  onOpen: (() => void) | null;
};

panel.onOpen = () => panel.update({ canvas: `${totalWidth}×${totalHeight} px` });

// Size the element to the natural Spout input resolution on first connect.
const el = document.querySelector("spout-video") as HTMLElement;
el.addEventListener("spout-connect", (e) => {
  const { width, height } = (e as CustomEvent).detail;
  el.style.width = `${width}px`;
  el.style.height = `${height}px`;
});
