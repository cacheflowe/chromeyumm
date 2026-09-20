// Tiny local settings server — stands in for localStorage, which doesn't
// survive a Chromeyumm relaunch (CEF's Chrome-runtime fails to create a
// persistent profile for a nested request-context partition; see the
// project's chat history / native/shared/app_paths.h for the full story).
// Settings are written straight to a JSON file on disk instead, so they
// survive across restarts regardless of the browser's own storage state.
//
// Run alongside the Vite dev server (see package.json's "dev" script, which
// starts both). The ticker page talks to this over plain fetch() — it works
// the same whether the page itself is served by Vite or a future static
// build, since this is a separate, fixed local port.

import { existsSync, mkdirSync } from "fs";
import { join } from "path";

const PORT = 5177;
const DATA_DIR = join(import.meta.dir, "data");
const SETTINGS_FILE = join(DATA_DIR, "settings.json");

if (!existsSync(DATA_DIR)) mkdirSync(DATA_DIR, { recursive: true });

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type",
};

Bun.serve({
  port: PORT,
  async fetch(req) {
    const url = new URL(req.url);

    if (req.method === "OPTIONS") {
      return new Response(null, { headers: CORS_HEADERS });
    }

    if (url.pathname === "/api/settings" && req.method === "GET") {
      const file = Bun.file(SETTINGS_FILE);
      if (!(await file.exists())) {
        return Response.json(null, { headers: CORS_HEADERS });
      }
      return new Response(file, {
        headers: { ...CORS_HEADERS, "Content-Type": "application/json" },
      });
    }

    if (url.pathname === "/api/settings" && req.method === "POST") {
      const body = await req.text();
      try {
        JSON.parse(body);
      } catch {
        return new Response("Invalid JSON", { status: 400, headers: CORS_HEADERS });
      }
      await Bun.write(SETTINGS_FILE, body);
      return new Response("OK", { headers: CORS_HEADERS });
    }

    return new Response("Not found", { status: 404, headers: CORS_HEADERS });
  },
});

console.log(`[p5-ticker] settings server → http://localhost:${PORT} (file: ${SETTINGS_FILE})`);
