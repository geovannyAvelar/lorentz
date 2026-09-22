// Generic reverse proxy: /api/lorentz/<anything> -> LORENTZ_API_URL/api/<anything>.
// The browser only ever talks to this app; this route makes the actual,
// server-to-server call to Lorentz and attaches the session (as a "sid"
// header, never as a cookie - see lib/config.ts for why that matters).
import { NextRequest, NextResponse } from "next/server";
import { callLorentz } from "@/lib/lorentz-server";
import { SESSION_COOKIE } from "@/lib/config";

const NO_BODY_METHODS = new Set(["GET", "HEAD"]);
// Statuses that must not carry a body (HTTP/1.1 forbids it for these).
const NO_BODY_STATUS = new Set([204, 304]);

async function handle(
  request: NextRequest,
  { params }: { params: Promise<{ path: string[] }> },
) {
  const { path } = await params;
  // Segments were already URL-decoded by Next's router; re-encode each one on
  // its own so a domain or regex containing "/", "?" or "#" round-trips
  // through this proxy the same way it would reach Lorentz directly.
  const target = `/api/${path.map(encodeURIComponent).join("/")}`;
  const sid = request.cookies.get(SESSION_COOKIE)?.value;
  const body = NO_BODY_METHODS.has(request.method)
    ? undefined
    : await request.text();

  const { status, body: responseBody } = await callLorentz(target, {
    method: request.method,
    sid,
    body: body || undefined,
    search: request.nextUrl.search,
  });

  if (NO_BODY_STATUS.has(status)) {
    const res = new NextResponse(null, { status });
    if (status === 401) res.cookies.delete(SESSION_COOKIE);
    return res;
  }

  const res = NextResponse.json(responseBody ?? {}, { status });
  // A session that Lorentz no longer honors (expired, the account was
  // disabled, ...) is not worth keeping around client-side either.
  if (status === 401) res.cookies.delete(SESSION_COOKIE);
  return res;
}

export {
  handle as GET,
  handle as POST,
  handle as PUT,
  handle as PATCH,
  handle as DELETE,
};
