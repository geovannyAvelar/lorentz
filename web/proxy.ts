// Keeps a signed-out visitor off every page except /login, before any of
// them render. This is a convenience redirect only - the cookie's mere
// presence is checked, not its validity - the real enforcement is Lorentz's
// own 401/403 on every API call, which the pages already react to.
//
// Named proxy.ts, not middleware.ts: Next.js 16 renamed the file convention
// (same behavior, see node_modules/next/dist/docs/.../proxy.md).
import { NextRequest, NextResponse } from "next/server";
import { SESSION_COOKIE } from "@/lib/config";

const PUBLIC_PATHS = new Set(["/login"]);

export default function proxy(request: NextRequest) {
  const { pathname } = request.nextUrl;
  if (PUBLIC_PATHS.has(pathname)) return NextResponse.next();

  if (!request.cookies.get(SESSION_COOKIE)) {
    const url = request.nextUrl.clone();
    url.pathname = "/login";
    url.search = "";
    if (pathname !== "/") url.searchParams.set("next", pathname);
    return NextResponse.redirect(url);
  }
  return NextResponse.next();
}

export const config = {
  // Everything except the proxy's own API routes, Next's internal assets and
  // the favicon - those must load before we know whether anyone is logged in.
  matcher: ["/((?!api/|_next/static|_next/image|favicon.ico).*)"],
};
