import { parseLayoutParams } from "../../components/layout-params.js";
import "../../components/debug-panel.js";
import "../../components/spout-receiver.js";

const { totalWidth, totalHeight } = parseLayoutParams();

const panel = document.querySelector("debug-panel") as HTMLElement & {
  update(s: { canvas?: string }): void;
  onOpen: (() => void) | null;
};

panel.onOpen = () => panel.update({ canvas: `${totalWidth}×${totalHeight} px` });
