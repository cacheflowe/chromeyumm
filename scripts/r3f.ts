/**
 * Start the R3F Vite dev server and chromeyumm browser together.
 * Usage: bun run demo
 */

const vite = Bun.spawn(["bun", "run", "dev"], {
  cwd: "dist/views/r3f",
  stdio: ["inherit", "inherit", "inherit"],
});

const browser = Bun.spawn(["dist/chromeyumm.exe"], {
  stdio: ["inherit", "inherit", "inherit"],
});

await Promise.all([vite.exited, browser.exited]);
