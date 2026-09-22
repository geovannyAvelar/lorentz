// Login, logout and "who am I" - the three operations that need to touch the
// httpOnly session cookie, kept apart from the generic proxy in
// app/api/lorentz for that reason.
import { NextRequest, NextResponse } from "next/server";
import { callLorentz } from "@/lib/lorentz-server";
import { SESSION_COOKIE } from "@/lib/config";

interface LorentzSession {
  valid: boolean;
  totp: boolean;
  sid?: string;
  validity: number;
  message: string | null;
  user: { id: number; username: string; role: "admin" | "viewer" } | null;
}

const INVALID_SESSION: { session: LorentzSession } = {
  session: {
    valid: false,
    totp: false,
    validity: -1,
    message: null,
    user: null,
  },
};

// GET: report the current session, if any. Always answers 200 - "not logged
// in" is a normal state, not an error - with session.valid telling the two apart.
export async function GET(request: NextRequest) {
  const sid = request.cookies.get(SESSION_COOKIE)?.value;
  if (!sid) return NextResponse.json(INVALID_SESSION);

  const { status, body } = await callLorentz<{ session: LorentzSession }>(
    "/api/auth",
    { sid },
  );
  if (status !== 200 || !body) {
    const res = NextResponse.json(INVALID_SESSION);
    res.cookies.delete(SESSION_COOKIE);
    return res;
  }
  return NextResponse.json(body);
}

// POST: log in. Body is {username?, password, totp?}; username is left out to
// log in with the password of the configuration (webserver.api.password).
export async function POST(request: NextRequest) {
  let payload: Record<string, unknown>;
  try {
    payload = await request.json();
  } catch {
    return NextResponse.json(
      { error: { key: "bad_request", message: "Invalid JSON body" } },
      { status: 400 },
    );
  }

  if (typeof payload.password !== "string") {
    return NextResponse.json(
      { error: { key: "bad_request", message: "password is required" } },
      { status: 400 },
    );
  }

  const forward: Record<string, unknown> = { password: payload.password };
  if (typeof payload.username === "string" && payload.username.trim() !== "")
    forward.username = payload.username.trim();
  if (typeof payload.totp === "number") forward.totp = payload.totp;

  const { status, body } = await callLorentz<{ session: LorentzSession }>(
    "/api/auth",
    { method: "POST", body: JSON.stringify(forward) },
  );

  const session = body?.session;
  const sid = status === 200 ? session?.sid : undefined;
  // The sid never needs to reach the browser: this app is the only thing
  // that uses it, and it lives in the httpOnly cookie set below instead.
  // Delete it before serializing the response, not after.
  if (session) delete session.sid;

  const res = NextResponse.json(body ?? INVALID_SESSION, { status });
  if (sid && session?.valid) {
    res.cookies.set(SESSION_COOKIE, sid, {
      httpOnly: true,
      sameSite: "lax",
      secure: request.nextUrl.protocol === "https:",
      path: "/",
    });
  }
  return res;
}

// DELETE: log out.
export async function DELETE(request: NextRequest) {
  const sid = request.cookies.get(SESSION_COOKIE)?.value;
  if (sid) await callLorentz("/api/auth", { method: "DELETE", sid });
  const res = new NextResponse(null, { status: 204 });
  res.cookies.delete(SESSION_COOKIE);
  return res;
}
