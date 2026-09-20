# p5 Ticker

A scrolling LED-matrix text ticker built with [p5.js](https://p5js.org/), ported from a
client's React/p5 app down to just the p5 parts. Hardcoded to a fixed sketch resolution
(`384×32`, matching a physical LED panel), with plain-HTML controls for scroll/position/
font/color tuning, persisted to `localStorage`.

## Run it

```bash
bun run demo-p5-ticker   # from repo root — starts the Vite dev server on :5176
```

or, from this directory: `bun run dev`.

Press **`h`** to hide/show the controls panel.

## How it works

### Layout: sketch + panel, not sketch + browser chrome

The canvas is hardcoded in `src/main.ts` (`SKETCH_WIDTH`/`SKETCH_HEIGHT`, currently
`384×32`) and pinned to the top-left of the window (`position: fixed; top:0; left:0`).
The controls panel (`#controls` in `index.html`) starts at `top: 32px` — i.e. exactly
where the sketch ends — and fills the rest of the window. **The two must stay in sync**;
if you change `SKETCH_HEIGHT`, update `#controls`'s `top` to match (there's a comment at
each spot pointing at the other).

This split matters for Chromeyumm's `display-config.json`: set `virtualCanvas` to the
full page size (sketch + panel), and give the LED-panel output window a `source` rect
that crops out *just* the `384×32` sketch region, e.g.:

```jsonc
{
  "virtualCanvas": { "width": 384, "height": 500 },
  "windows": [
    {
      "window": { "x": 0, "y": 0, "width": 384, "height": 32 },
      "source": { "x": 0, "y": 0, "width": 384, "height": 32 }
    }
  ]
}
```

The D3D blit that ships pixels to a physical output window is a direct, unscaled copy
(`CopySubresourceRegion` in `native/cef-wrapper.cpp`), so `window` and `source` must be
the same size — you can't stretch. In Chromeyumm's **interactive mode** (`Ctrl+M`) you
see the full, uncropped page (sketch + controls); in output mode, only the cropped
`source` rect goes out to the physical display.

### No offscreen graphics buffer

Unlike the source React app (which rendered text to an offscreen `p5.Graphics` before
compositing), everything here draws straight to the main canvas. Each frame:

1. `p.clear()` — canvas goes fully transparent.
2. Draw the ticker text in white.
3. If the threshold shader is enabled, `p.filter(thresholdShader)` re-colors it and
   hard-clips the alpha.

Wherever the canvas alpha ends up at 0, the page's `background-color` shows through —
so "background color" is just a CSS color (set on `<body>`), not a second draw pass.

### The threshold shader

`src/glsl/text-threshold.ts` converts p5's anti-aliased text edges into hard on/off
alpha, which reads much better on physical LED panels than soft edges. One important
gotcha if you touch this shader: **p5's WebGL filter compositing blends with
premultiplied alpha.** Naively outputting `vec4(uTextColor, alpha)` leaks the text color
into transparent background pixels, turning the whole canvas solid. The fix is to
premultiply the output: `vec4(uTextColor * alpha, alpha)`. (Diagnosed by visualizing the
raw alpha channel in isolation — it was correct — then bisecting the shader output until
the leak reproduced with a hardcoded color.)

### Controls: no native popups

`<select>` and `<input type="color">` both open native OS popups for their dropdown
list / color picker dialog. CEF's off-screen-rendered (OSR) content — which is how
Chromeyumm renders everything — **can't display those popups.** They aren't just
invisible either: clicking to open one still opens the real, unseen popup, which then
swallows all keyboard input (including arrow keys) until dismissed — and you can't
dismiss what you can't see. In Chromeyumm's interactive mode (`Ctrl+M`) there's an
actual OS window to click away from and recover; in output mode there isn't.

So this UI avoids both entirely:

- **Font picker** — a row of plain buttons (`.option-row` / `.option-btn` in
  `index.html`), each writing straight into a hidden `<input>` and firing the same
  `input` event a real field would. See `initControls()`'s `buttonGroups` in
  `src/controls.ts` — it's a small generic system also used for colors.
- **Color pickers** — a hex text `<input>` plus a row of preset swatch buttons
  (`.swatch-row` / `.swatch`), same wiring pattern. A live preview square next to the
  hex field shows the current color since you can't rely on `<input type="color">`'s
  own swatch.

If you add more font options, make sure the value has **no comma-separated fallback
list**: p5's `Renderer2D._applyTextProperties()` wraps the *entire* font string in one
pair of quotes whenever it contains whitespace, so `"Arial, Helvetica, sans-serif"`
becomes one bogus literal font name instead of a fallback chain. Use a single font name
(e.g. `"Arial"`, `"Trebuchet MS"`).

### Persistence

All settings save to `localStorage` (`p5-ticker-settings`) on every change and reload on
startup. **This does work in Chromeyumm** — its CEF `cache_path` points at a persistent
per-app user-data directory (`native/cef-wrapper.cpp`), not an in-memory/incognito
profile, so it's real disk storage that survives restarts.

### The text content

`TEXTS` in `src/main.ts` is a hardcoded array of strings — there's no UI for editing
ticker content, just edit the array directly.

## Controls reference

| Control | Notes |
|---|---|
| font | button row (see above); add custom fonts via `@font-face` — see below |
| font size, kerning, y offset | kerning is manual, p5's `text()` has no built-in letter-spacing |
| repeat padding | gap inserted between repeated strings as they scroll |
| scroll speed | signed; negative scrolls right-to-left |
| threshold enabled / threshold | toggles the crisp-edge shader pass and its cutoff (0–1) |
| text color / bg color | hex input + swatches; bg color is applied as the page's CSS background |
| reset | restores all defaults and re-saves them |

## Adding a custom font

p5's 2D-mode `textFont()` just sets the canvas's `font` property — any regular web font
works, no p5-specific font loading needed:

1. Drop the font file in `public/fonts/` (Vite serves `public/` at the site root).
2. Add an `@font-face` rule in `index.html`'s `<style>` block pointing at it.
3. Add a button to the `.option-row[data-for="fontFamily"]` group with
   `data-value="Your Font Name"` matching the `font-family` in the `@font-face` rule.

Self-host the file rather than linking a CDN (e.g. Google Fonts) — this may run offline
on a kiosk/LED install with no internet access. `Archivo Black` in this repo is an
example: downloaded once from `fonts.gstatic.com` into `public/fonts/`, declared via
`@font-face`, and its button is styled with its own font-family so it previews itself.

No extra "wait for the font to load" logic is needed — `p.draw()` redraws continuously,
so the correct font appears within a frame or two of the browser finishing the (async)
font load.
