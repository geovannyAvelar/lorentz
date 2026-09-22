// Is the API still open, i.e. does no account and no configured password
// exist yet? GET /api/users answers 200 without authentication exactly in
// that state (see users_login_required() in database/user-table.c) and 401
// otherwise, so that single unauthenticated call is enough to tell.
import { NextResponse } from "next/server";
import { callLorentz } from "@/lib/lorentz-server";

export async function GET() {
  const { status } = await callLorentz("/api/users");
  return NextResponse.json({ open: status === 200 });
}
