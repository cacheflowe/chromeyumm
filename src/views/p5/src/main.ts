import p5 from "p5";
import Matter from "matter-js";

// ---------------------------------------------------------------------------
// p5.js + Matter.js physics playground
//
// Colorful shapes rain down, bounce off angled platforms, and pile up.
// Click anywhere to spawn a burst of shapes.
// ---------------------------------------------------------------------------

const { Engine, Bodies, Body, Composite } = Matter;

const palette = ["#ff2255", "#2255ff", "#00ffaa", "#ff8800", "#7b00e0", "#00ccdd"];
// const palette = ["#333", "#666", "#999", "#bbb", "#eee"];
const MAX_SHAPES = 300;
const SPAWN_MS = 60;

interface RenderData {
  kind: "circle" | "rect" | "poly";
  r?: number;
  color: string;
}

new p5((p: p5) => {
  let engine: Matter.Engine;
  const shapes: Matter.Body[] = [];
  let lastSpawn = 0;

  function reset() {
    // Remove all dynamic bodies
    for (const body of shapes) {
      Composite.remove(engine.world, body);
    }
    shapes.length = 0;
    lastSpawn = 0;
  }

  // Scale factor — everything sizes relative to the smaller canvas dimension.
  // At 384px this is ~1.0; at 32px shapes shrink proportionally.
  let unit = 1;

  // ----- setup --------------------------------------------------------------

  p.setup = () => {
    p.createCanvas(p.windowWidth, p.windowHeight);
    unit = Math.min(p.width, p.height) / 96;
    engine = Engine.create({ gravity: { x: 0, y: 0.6, scale: 0.001 } });

    // Ground + walls (oversized so nothing escapes)
    const wallThick = 50 * unit;
    addStatic(p.width / 2, p.height + wallThick / 2, p.width + 200 * unit, wallThick, 0);
    addStatic(-wallThick / 2, p.height / 2, wallThick, p.height + 200 * unit, 0);
    addStatic(p.width + wallThick / 2, p.height / 2, wallThick, p.height + 200 * unit, 0);

    // Angled platforms — shapes bounce and scatter
    // const platW = p.width * 0.7;
    // const platH = Math.max(4, 14 * unit);
    // addStatic(p.width * 0.25, p.height * 0.5, platW, platH, 0.2);
    // addStatic(p.width * 0.75, p.height * 0.35, platW * 0.9, platH, -0.25);
    // addStatic(p.width * 0.5, p.height * 0.7, platW * 0.85, platH, 0.15);
  };

  // ----- draw ----------------------------------------------------------------

  p.draw = () => {
    Engine.update(engine, 1000 / 60);

    p.background(12, 14, 22);

    // Auto-spawn from top
    const now = p.millis();
    const margin = 80 * unit;
    if (now - lastSpawn > SPAWN_MS && shapes.length < MAX_SHAPES) {
      spawn(p.random(margin, p.width - margin), p.random(-margin, -margin * 0.12));
      lastSpawn = now;
    }

    // Cull bodies that fell offscreen
    for (let i = shapes.length - 1; i >= 0; i--) {
      if (shapes[i].position.y > p.height + 200 * unit) {
        Composite.remove(engine.world, shapes[i]);
        shapes.splice(i, 1);
      }
    }

    // Static platforms
    p.noStroke();
    p.fill(35, 40, 55);
    for (const body of Composite.allBodies(engine.world)) {
      if (body.isStatic) drawVerts(body);
    }

    // Dynamic shapes
    for (const body of shapes) {
      const rd = (body as any)._rd as RenderData | undefined;
      if (!rd) continue;
      p.stroke(0);
      p.strokeWeight(1);
      p.fill(rd.color);
      if (rd.kind === "circle") {
        p.circle(body.position.x, body.position.y, rd.r! * 2);
      } else {
        drawVerts(body);
      }
    }
  };

  // ----- interaction ---------------------------------------------------------

  p.keyPressed = () => {
    if (p.key === "r" || p.key === "R") reset();
  };

  p.mousePressed = () => {
    const spread = 50 * unit;
    for (let i = 0; i < 10; i++) {
      spawn(p.mouseX + p.random(-spread, spread), p.mouseY + p.random(-spread, spread));
    }
  };

  p.windowResized = () => {
    p.resizeCanvas(p.windowWidth, p.windowHeight);
  };

  // ----- helpers -------------------------------------------------------------

  function addStatic(x: number, y: number, w: number, h: number, angle: number) {
    Composite.add(engine.world, Bodies.rectangle(x, y, w, h, { isStatic: true, angle }));
  }

  function spawn(x: number, y: number) {
    const color = palette[Math.floor(p.random(palette.length))];
    const roll = p.random();
    let body: Matter.Body;

    if (roll < 0.4) {
      // Circle
      const r = p.random(8, 24) * unit;
      body = Bodies.circle(x, y, r, { restitution: 0.65, friction: 0.05 });
      (body as any)._rd = { kind: "circle", r, color } satisfies RenderData;
    } else if (roll < 0.7) {
      // Rectangle
      const w = p.random(14, 36) * unit;
      const h = p.random(14, 36) * unit;
      body = Bodies.rectangle(x, y, w, h, { restitution: 0.5, friction: 0.1 });
      (body as any)._rd = { kind: "rect", color } satisfies RenderData;
    } else {
      // Polygon (3–6 sides)
      const sides = Math.floor(p.random(3, 7));
      const r = p.random(10, 22) * unit;
      body = Bodies.polygon(x, y, sides, r, { restitution: 0.95, friction: 0.01 });
      (body as any)._rd = { kind: "poly", color } satisfies RenderData;
    }

    Body.setVelocity(body, { x: p.random(-2, 2) * unit, y: p.random(0, 2) * unit });
    Body.setAngularVelocity(body, p.random(-0.05, 0.05));
    Composite.add(engine.world, body);
    shapes.push(body);
  }

  function drawVerts(body: Matter.Body) {
    p.beginShape();
    for (const v of body.vertices) p.vertex(v.x, v.y);
    p.endShape(p.CLOSE);
  }
});
