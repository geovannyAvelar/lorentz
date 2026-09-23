#!/usr/bin/env node
// Runs after `next build` (see package.json's "build:embed"). Lorentz's
// Content-Security-Policy forbids inline scripts by default; rather than
// weakening it with 'unsafe-inline', webui_handler() (src/api/webui/webui.c)
// sends a script-src augmented with a 'sha256-...' entry per inline
// <script> Next.js's static export embeds (hydration bootstrap, Mantine's
// color-scheme initializer) - this script computes those hashes from the
// exact built output and writes them next to it for generate.sh to pick up.
import { createHash } from "node:crypto";
import { readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.resolve(scriptDir, "..", "out");

// Inline <script>...</script> blocks: anything without a "src=" attribute.
const INLINE_SCRIPT_RE = /<script(?![^>]*\ssrc=)[^>]*>([\s\S]*?)<\/script>/g;

async function htmlFiles(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) files.push(...(await htmlFiles(full)));
    else if (entry.name.endsWith(".html")) files.push(full);
  }
  return files;
}

const hashesByFile = {};
for (const file of await htmlFiles(outDir)) {
  const html = await readFile(file, "utf8");
  const hashes = new Set();
  for (const match of html.matchAll(INLINE_SCRIPT_RE)) {
    const body = match[1];
    if (body.length === 0) continue;
    const digest = createHash("sha256").update(body, "utf8").digest("base64");
    hashes.add(`'sha256-${digest}'`);
  }
  if (hashes.size > 0) {
    hashesByFile[path.relative(outDir, file).split(path.sep).join("/")] = [...hashes];
  }
}

await writeFile(path.join(outDir, ".csp-hashes.json"), JSON.stringify(hashesByFile));
console.log(`Computed CSP script hashes for ${Object.keys(hashesByFile).length} file(s)`);
