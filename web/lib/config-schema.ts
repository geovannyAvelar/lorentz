import type { ConfigItemMeta, ConfigNode, ConfigOption } from "./types";

// One leaf of the configuration tree, flattened: "dns.cache.size" is
// section "dns", group "cache", leaf "size".
export interface ConfigEntry {
  key: string;
  section: string;
  group: string;
  leaf: string;
  meta: ConfigItemMeta;
}

// A leaf carries these; a subsection (an object of more nodes) does not.
function isItem(node: ConfigItemMeta | ConfigNode): node is ConfigItemMeta {
  return typeof node.type === "string" && "flags" in node && "description" in node;
}

export function flattenConfig(tree: ConfigNode): ConfigEntry[] {
  const entries: ConfigEntry[] = [];
  const walk = (node: ConfigNode, path: string[]) => {
    for (const [name, child] of Object.entries(node)) {
      const next = [...path, name];
      if (isItem(child)) {
        entries.push({
          key: next.join("."),
          section: next[0],
          group: next.slice(1, -1).join("."),
          leaf: name,
          meta: child,
        });
      } else {
        walk(child, next);
      }
    }
  };
  walk(tree, []);
  return entries;
}

// {"dns.cache.size": 5000} -> {dns: {cache: {size: 5000}}}, the body of
// PATCH /api/config
export function buildPatch(changes: Record<string, unknown>): ConfigNode {
  const root: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(changes)) {
    const path = key.split(".");
    let node = root;
    for (const part of path.slice(0, -1)) {
      node = (node[part] ??= {}) as Record<string, unknown>;
    }
    node[path[path.length - 1]] = value;
  }
  return root as ConfigNode;
}

export function isPassword(meta: ConfigItemMeta): boolean {
  return meta.type.startsWith("password");
}

// String keys that hold a secret. files.database is also caught by its
// value below, since it is a plain path until pointed at PostgreSQL.
const SECRET_KEYS = new Set([
  "webserver.api.pwhash",
  "webserver.api.app_pwhash",
  "webserver.api.totp_secret",
]);

// scheme://user:password@host/... - postgresql://lorentz:secret@db/lorentz
const URI_WITH_PASSWORD = /^[a-z][a-z0-9+.-]*:\/\/[^\s/@:]*:[^\s/@]+@/i;

// Judged on the saved value, not the one being typed, so the input does not
// change kind halfway through an edit.
export function isSecretString(entry: ConfigEntry): boolean {
  return (
    SECRET_KEYS.has(entry.key) ||
    (typeof entry.meta.value === "string" && URI_WITH_PASSWORD.test(entry.meta.value))
  );
}

// The value shown while nothing was edited. A write-only password is never
// sent back by the API (it reads "********"), so it starts empty.
export function originalValue(meta: ConfigItemMeta): unknown {
  return isPassword(meta) ? "" : meta.value;
}

export function isSame(meta: ConfigItemMeta, next: unknown): boolean {
  return JSON.stringify(originalValue(meta)) === JSON.stringify(next);
}

export function options(meta: ConfigItemMeta): ConfigOption[] {
  return Array.isArray(meta.allowed) ? meta.allowed : [];
}

// Descriptions run to several paragraphs; the first is enough under a field.
export function firstParagraph(text: string): string {
  return text.trim().split(/\n\s*\n/)[0].replace(/\s*\n\s*/g, " ");
}
