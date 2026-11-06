import { createReadStream, existsSync, statSync } from "node:fs";
import { extname, join, normalize } from "node:path";
import { createServer } from "node:http";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL(".", import.meta.url));
const port = Number(process.env.PORT || 4173);
const types = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".txt": "text/plain; charset=utf-8",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".webp": "image/webp",
  ".wasm": "application/wasm"
};

createServer((request, response) => {
  const pathname = decodeURIComponent(new URL(request.url, `http://${request.headers.host}`).pathname);
  const requested = pathname === "/" ? "/index.html" : pathname;
  const resolved = normalize(join(root, requested));
  if (!resolved.startsWith(root) || !existsSync(resolved) || !statSync(resolved).isFile()) {
    response.writeHead(404).end("Not found");
    return;
  }
  response.setHeader("Content-Type", types[extname(resolved)] || "application/octet-stream");
  response.setHeader("Cache-Control", "no-store");
  createReadStream(resolved).pipe(response);
}).listen(port, "127.0.0.1", () => {
  console.log(`VexDB Lite WASM: http://127.0.0.1:${port}`);
});
