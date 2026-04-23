import p5 from "p5";

const canvasW = 32;
const canvasH = 384;
const NUM_PARTICLES = 50;
const TURN_CHANCE = 0.05;

// N, E, S, W
const DIRS = [
  [0, -1],
  [1, 0],
  [0, 1],
  [-1, 0],
] as const;

const palette = ["#ff2255", "#2255ff", "#00ffaa", "#ff8800", "#7b00e0", "#00ccdd"];
// const palette = ["#ffffff"];
// const palette = ["#00ff00"];

interface Particle {
  x: number;
  y: number;
  dir: number;
  color: string;
}

new p5((p: p5) => {
  let particles: Particle[] = [];

  function initParticles() {
    particles = [];
    for (let i = 0; i < NUM_PARTICLES; i++) {
      particles.push({
        x: Math.floor(p.random((canvasW - 2) / 2)) * 2,
        y: Math.floor(p.random((canvasH - 2) / 2)) * 2,
        dir: Math.floor(p.random(4)),
        color: palette[Math.floor(p.random(palette.length))],
      });
    }
  }

  // Pick randomly from bounce or either 90° turn when hitting a wall
  function wallTurn(bounceDir: number, turnA: number, turnB: number): number {
    const r = p.random();
    if (r < 0.33) return turnA;
    if (r < 0.66) return turnB;
    return bounceDir;
  }

  p.setup = () => {
    p.createCanvas(canvasW + 16, canvasH);
    p.background(12, 14, 22);
    p.noSmooth();
    p.noStroke();
    initParticles();
  };

  p.draw = () => {
    // Fade trail with semi-transparent overlay
    // p.fill(12, 14, 22, 26);
    p.fill(0, 0, 0, 36);
    p.rect(0, 0, canvasW, canvasH);

    p.noStroke();
    for (const pt of particles) {
      if (p.random() < TURN_CHANCE) {
        pt.dir = (pt.dir + (p.random() < 0.5 ? 1 : 3)) % 4;
      }

      const [dx, dy] = DIRS[pt.dir];
      pt.x += dx * 2;
      pt.y += dy * 2;

      if (pt.x < 0) {
        pt.x = 0;
        pt.dir = wallTurn(1, 0, 2); // bounce E, or turn N/S
      } else if (pt.x > canvasW - 2) {
        pt.x = canvasW - 2;
        pt.dir = wallTurn(3, 0, 2); // bounce W, or turn N/S
      }
      if (pt.y < 0) {
        pt.y = 0;
        pt.dir = wallTurn(2, 1, 3); // bounce S, or turn E/W
      } else if (pt.y > canvasH - 2) {
        pt.y = canvasH - 2;
        pt.dir = wallTurn(0, 1, 3); // bounce N, or turn E/W
      }

      p.fill(pt.color);
      p.rect(pt.x, pt.y, 2, 2);
    }

    p.image(p.get(0, 0, canvasW, canvasH), 32, 0, canvasW / 2, canvasH / 2);
  };

  p.keyPressed = () => {
    if (p.key === "r" || p.key === "R") {
      p.background(12, 14, 22);
      initParticles();
    }
  };
});
