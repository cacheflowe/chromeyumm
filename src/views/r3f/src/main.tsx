import { createRoot } from "react-dom/client";
import { App } from "./App";
import "../../../components/debug-panel.js";
import "../../../components/slot-overlay.js";

createRoot(document.getElementById("root")!).render(<App />);
