"use client";

import useSWR from "swr";
import type { Session } from "@/lib/types";
import { setCsrfToken } from "@/lib/client";

export const SESSION_KEY = "session";

export async function fetchSession(): Promise<{ session: Session }> {
  const res = await fetch("/api/auth", { cache: "no-store" });
  const body: { session: Session } = await res.json();
  setCsrfToken(body.session?.csrf);
  return body;
}

export function useSession() {
  const { data, error, isLoading, mutate } = useSWR(SESSION_KEY, fetchSession, {
    refreshInterval: 60_000,
    revalidateOnFocus: true,
  });

  const session = data?.session;
  // A login with the password of the configuration, or no login at all while
  // the API is open, carries no account and has the rights of an admin - see
  // api/auth.c and api/users.c on the Lorentz side (is_admin() there follows
  // the same rule). `== null` (not `===`) on purpose: it also covers a
  // session response that leaves "user" out entirely rather than nulling it.
  const isAdmin = Boolean(session?.valid) && (session?.user == null || session.user.role === "admin");

  // While no account and no configured password exist, Lorentz answers "valid"
  // to everybody, without a session id (see get_session_object() in
  // src/api/auth.c). That is not a login, it is a fresh install still waiting
  // for its first password.
  const isOpen = Boolean(session?.valid) && session?.sid == null;

  return { session, isAdmin, isOpen, isLoading, error, mutate };
}
