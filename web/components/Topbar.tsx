"use client";

import Link from "next/link";
import { useRouter } from "next/navigation";
import { useState } from "react";
import { Button, Badge, Group, UnstyledButton, Text } from "@mantine/core";
import { IconLogout } from "@tabler/icons-react";
import type { Session } from "@/lib/types";
import { setCsrfToken } from "@/lib/client";

export function Topbar({ session }: { session: Session | undefined }) {
  const router = useRouter();
  const [loggingOut, setLoggingOut] = useState(false);

  async function logout() {
    setLoggingOut(true);
    await fetch("/api/auth", { method: "DELETE" });
    setCsrfToken(null);
    router.replace("/login");
    router.refresh();
  }

  const label = session?.user
    ? session.user.username
    : session?.valid
      ? "configured password"
      : "";

  return (
    <Group h="100%" px="md" justify="space-between">
      <Text fw={600}>Lorentz</Text>
      <Group gap="sm">
        {label && (
          <UnstyledButton component={Link} href="/account">
            <Group gap="xs">
              <Text size="sm">{label}</Text>
              <Badge color={session?.user?.role === "viewer" ? "gray" : "blue"}>
                {session?.user?.role ?? "admin"}
              </Badge>
            </Group>
          </UnstyledButton>
        )}
        <Button
          size="xs"
          variant="default"
          leftSection={<IconLogout size={16} />}
          onClick={logout}
          loading={loggingOut}
        >
          Sign out
        </Button>
      </Group>
    </Group>
  );
}
