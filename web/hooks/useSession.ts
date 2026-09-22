"use client";

import useSWR from "swr";
import type { Session } from "@/lib/types";

async function fetchSession(): Promise<{ session: Session }> {
  const res = await fetch("/api/session", { cache: "no-store" });
  return res.json();
}

export function useSession() {
  const { data, error, isLoading, mutate } = useSWR("session", fetchSession, {
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

  return { session, isAdmin, isLoading, error, mutate };
}
