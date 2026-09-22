"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useState } from "react";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import type { Session } from "@/lib/types";

export function Topbar({ session }: { session: Session | undefined }) {
  const router = useRouter();
  const [loggingOut, setLoggingOut] = useState(false);

  async function logout() {
    setLoggingOut(true);
    await fetch("/api/session", { method: "DELETE" });
    router.replace("/login");
    router.refresh();
  }

  const label = session?.user
    ? session.user.username
    : session?.valid
      ? "configured password"
      : "";

  return (
    <header className="flex h-14 shrink-0 items-center justify-between border-b border-border bg-surface px-4">
      <div />
      <div className="flex items-center gap-3">
        {label && (
          <Link href="/account" className="flex items-center gap-2 text-sm text-foreground hover:underline">
            {label}
            <Badge tone={session?.user?.role === "viewer" ? "neutral" : "accent"}>
              {session?.user?.role ?? "admin"}
            </Badge>
          </Link>
        )}
        <Button size="sm" onClick={logout} loading={loggingOut}>
          Sign out
        </Button>
      </div>
    </header>
  );
}
