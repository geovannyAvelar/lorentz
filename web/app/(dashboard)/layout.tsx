"use client";

import { useEffect } from "react";
import { useRouter } from "next/navigation";
import type { ReactNode } from "react";
import { AppShell, Center, Loader } from "@mantine/core";
import { useSession } from "@/hooks/useSession";
import { Sidebar } from "@/components/Sidebar";
import { Topbar } from "@/components/Topbar";

export default function DashboardLayout({ children }: { children: ReactNode }) {
  const { session, isAdmin, isOpen, isLoading } = useSession();
  const router = useRouter();

  useEffect(() => {
    // A signed-out visitor (the session expired, or the account was disabled
    // or deleted) goes to the login page, and so does anybody on a fresh
    // install: an open API lets everyone in, so the first thing to do there
    // is to set the password, on the same page.
    if (!isLoading && session && (!session.valid || isOpen)) {
      router.replace("/login");
    }
  }, [isLoading, session, isOpen, router]);

  if (isLoading || !session) {
    return (
      <Center h="100vh">
        <Loader />
      </Center>
    );
  }

  if (!session.valid || isOpen) return null;

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
