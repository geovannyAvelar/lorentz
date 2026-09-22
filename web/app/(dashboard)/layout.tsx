"use client";

import { useEffect } from "react";
import { useRouter } from "next/navigation";
import type { ReactNode } from "react";
import { AppShell, Center, Loader } from "@mantine/core";
import { useSession } from "@/hooks/useSession";
import { Sidebar } from "@/components/Sidebar";
import { Topbar } from "@/components/Topbar";

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
      <Center h="100vh">
        <Loader />
      </Center>
    );
  }

  if (!session.valid) return null;

  return (
    <AppShell header={{ height: 60 }} navbar={{ width: 224, breakpoint: "sm" }} padding="lg">
      <AppShell.Header>
        <Topbar session={session} />
      </AppShell.Header>
      <AppShell.Navbar p="sm">
        <Sidebar isAdmin={isAdmin} />
      </AppShell.Navbar>
      <AppShell.Main>{children}</AppShell.Main>
    </AppShell>
  );
}
