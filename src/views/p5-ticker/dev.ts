// Runs the settings server and the Vite dev server together as one command.
// Plain shell "&" backgrounding doesn't work portably here — Bun's own
// script runner rejects it outright, and cmd.exe's "&" means "run next"
// rather than "run in background" — so this spawns both as real child
// processes instead and forwards Ctrl+C to both.

const procs = [
  Bun.spawn(["bun", "run", "server.ts"], { stdio: ["inherit", "inherit", "inherit"] }),
  Bun.spawn(["bunx", "vite"], { stdio: ["inherit", "inherit", "inherit"] }),
];

function shutdown() {
  for (const proc of procs) proc.kill();
  process.exit(0);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

await Promise.race(procs.map((proc) => proc.exited));
shutdown();
