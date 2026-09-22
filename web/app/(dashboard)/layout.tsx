"use client";

import { useEffect } from "react";
import { useRouter } from "next/navigation";
import type { ReactNode } from "react";
import { useSession } from "@/hooks/useSession";
import { Sidebar } from "@/components/Sidebar";
import { Topbar } from "@/components/Topbar";
import { Spinner } from "@/components/ui/Feedback";

export default function DashboardLayout({ children }: { children: ReactNode }) {
  const { session, isAdmin, isLoading } = useSession();
  const router = useRouter();

  useEffect(() => {
    // proxy.ts already keeps a signed-out visitor off these pages by cookie
    // presence; this catches the cookie going stale after the fact (the
    // session expired, or the account was disabled or deleted).
    if (!isLoading && session && !session.valid) {
      router.replace("/login");
    }
  }, [isLoading, session, router]);

  if (isLoading || !session) {
    return (
      <div className="flex min-h-dvh items-center justify-center">
        <Spinner className="h-6 w-6" />
      </div>
    );
  }

  if (!session.valid) return null;

  return (
    <div className="flex min-h-dvh">
      <Sidebar isAdmin={isAdmin} />
      <div className="flex min-w-0 flex-1 flex-col">
        <Topbar session={session} />
        <main className="flex-1 overflow-y-auto p-6">{children}</main>
      </div>
    </div>
  );
}
