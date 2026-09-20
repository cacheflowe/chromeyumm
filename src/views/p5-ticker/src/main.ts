import p5 from "p5";
import alphaThreshold from "./glsl/text-threshold";
import { initControls, type TickerSettings } from "./controls";

// ---------------------------------------------------------------------------
// Scrolling text ticker, ported from a client's React/p5 app down to just the
// p5 parts: hardcoded strings, scroll/position/font controls (plain HTML
// sliders instead of Leva), and the alpha-threshold shader that gives text a
// crisp LED-matrix edge instead of anti-aliased soft edges.
//
// Unlike the source app, everything is drawn straight to the main canvas —
// no offscreen p5.Graphics buffer. The canvas is cleared to transparent each
// frame and the page background shows through wherever the threshold shader
// leaves alpha at 0, so "background color" is just a CSS background-color.
// ---------------------------------------------------------------------------

// Matches the physical LED panel resolution — see display-config.json, where
// the window's `source` rect crops out exactly this region, anchored to the
// top-left of the virtual canvas.
const SKETCH_WIDTH = 384;
const SKETCH_HEIGHT = 32;

function hexToRgb01(hex: string): [number, number, number] {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex.trim());
  if (!m) return [1, 1, 1];
  return [parseInt(m[1], 16) / 255, parseInt(m[2], 16) / 255, parseInt(m[3], 16) / 255];
}

function positiveModulo(value: number, divisor: number) {
  return ((value % divisor) + divisor) % divisor;
}

new p5((p: p5) => {
  let thresholdShader: p5.Shader | null = null;
  let settings: TickerSettings;
  let scrollX = 0;

  p.setup = () => {
    const canvas = p.createCanvas(SKETCH_WIDTH, SKETCH_HEIGHT);
    canvas.parent(document.body);
    p.pixelDensity(1);
    p.noStroke();
    p.textAlign(p.LEFT, p.CENTER);

    thresholdShader = p.createFilterShader(alphaThreshold);

    const controls = initControls(() => {
      document.body.style.backgroundColor = settings.bgColor;
    });
    settings = controls.settings;
    document.body.style.backgroundColor = settings.bgColor;
  };

  // Manual per-character advance so we can support a kerning (letter-spacing)
  // control — p5's text() has no built-in letter-spacing. Uses fontWidth(),
  // not textWidth(): p5 v2 redefined textWidth() to return *tight ink
  // bounds* (actualBoundingBoxLeft + actualBoundingBoxRight) rather than the
  // old advance width. A space has no ink, so textWidth(" ") is 0 — using it
  // here collapses every space, running words together. fontWidth() is p5
  // v2's new name for the old (advance-width) behavior.
  function kernedWidth(str: string): number {
    let w = 0;
    for (const ch of str) w += p.fontWidth(ch);
    return w + settings.kerning * Math.max(0, str.length - 1);
  }

  function drawKerned(str: string, x: number, y: number) {
    let cx = x;
    for (const ch of str) {
      p.text(ch, cx, y);
      cx += p.fontWidth(ch) + settings.kerning;
    }
  }

  // Single string, split on "|" — lets any number of messages be edited from
  // one text field without touching code, and avoids commas, which are far
  // more likely to show up in a real ticker headline than a pipe character.
  function getTexts(): string[] {
    const texts = settings.textMessage
      .split("|")
      .map((s) => s.trim())
      .filter((s) => s.length > 0);
    return texts.length > 0 ? texts : ["?"];
  }

  function drawTicker(fillColor: string) {
    p.textSize(settings.textSize);
    p.textFont(settings.fontFamily);
    p.fill(fillColor);

    const runs = getTexts().map((str) => ({
      str,
      width: Math.max(1, Math.ceil(kernedWidth(str)) + settings.repeatPadding),
    }));
    const totalWidth = runs.reduce((sum, run) => sum + run.width, 0);
    const travel = positiveModulo(-scrollX, totalWidth);

    let runIndex = 0;
    let traversed = 0;
    while (runIndex < runs.length - 1 && traversed + runs[runIndex].width <= travel) {
      traversed += runs[runIndex].width;
      runIndex += 1;
    }

    let x = -(travel - traversed);
    const y = p.height / 2 + settings.textYOffset;

    while (x < p.width) {
      const run = runs[runIndex];
      if (x + run.width >= 0) {
        drawKerned(run.str, x, y);
      }
      x += run.width;
      runIndex = (runIndex + 1) % runs.length;
    }
  }

  p.draw = () => {
    if (!settings || !thresholdShader) return;

    scrollX += settings.scrollSpeed * (p.deltaTime / (1000 / 60));

    p.clear();

    if (settings.textThresholdEnabled) {
      // Draw in white first so the shader has a clean alpha mask to
      // threshold, then it recolors with uTextColor.
      drawTicker("#ffffff");
      thresholdShader.setUniform("uThreshold", settings.textThreshold);
      thresholdShader.setUniform("uTextColor", hexToRgb01(settings.textColor));
      p.filter(thresholdShader);
    } else {
      drawTicker(settings.textColor);
    }
  };
});
