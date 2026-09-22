// Talks to the Lorentz REST API from the server side (route handlers only).
// "server-only" makes the bundler refuse to pull this file, and LORENTZ_API_URL
// with it, into any client bundle.
import "server-only";
import { LORENTZ_API_URL } from "./config";

export interface LorentzResponse<T = unknown> {
  status: number;
  body: T | null;
}

interface CallOptions {
  method?: string;
  sid?: string;
  body?: string;
  search?: string;
}

// A single request/response round trip with Lorentz. The body, if any, is
// parsed as JSON when it looks like JSON and handed back as-is otherwise (a
// handful of Lorentz error paths - a 404 on an unmatched route, for instance -
// answer with a plain-text body).
export async function callLorentz<T = unknown>(
  path: string,
  { method = "GET", sid, body, search }: CallOptions = {},
): Promise<LorentzResponse<T>> {
  const headers = new Headers();
  if (sid) headers.set("sid", sid);
  if (body !== undefined) headers.set("Content-Type", "application/json");

  const url = `${LORENTZ_API_URL}${path}${search ?? ""}`;
  let res: Response;
  try {
    res = await fetch(url, { method, headers, body, cache: "no-store" });
  } catch (cause) {
    // Lorentz is unreachable (down, wrong LORENTZ_API_URL, network partition).
    // Surface this as a 502 the caller can show, rather than an uncaught
    // exception turning into Next.js's generic 500 page.
    console.error(`Cannot reach Lorentz at ${url}:`, cause);
    return {
      status: 502,
      body: {
        error: {
          key: "upstream_unreachable",
          message: "Could not reach the Lorentz API",
          hint: LORENTZ_API_URL,
        },
      } as T,
    };
  }

  const text = await res.text();
  let parsed: T | null = null;
  if (text.length > 0) {
    try {
      parsed = JSON.parse(text) as T;
    } catch {
      parsed = text as unknown as T;
    }
  }
  return { status: res.status, body: parsed };
}
