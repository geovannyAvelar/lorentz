"use client";

import { Suspense, useState } from "react";
import type { FormEvent } from "react";
import { useRouter, useSearchParams } from "next/navigation";
import useSWR from "swr";
import {
  Paper,
  Title,
  Text,
  TextInput,
  PasswordInput,
  Button,
  Alert,
  Center,
  Loader,
  Stack,
} from "@mantine/core";
import { IconAlertCircle } from "@tabler/icons-react";
import { ApiError, api, setCsrfToken } from "@/lib/client";
import type { Session } from "@/lib/types";

// Is the API still open, i.e. does no account and no configured password
// exist yet? GET /api/users answers 200 without authentication exactly in
// that state (see users_login_required() in database/user-table.c) and 401
// otherwise, so that single unauthenticated call is enough to tell.
async function fetchBootstrap(): Promise<{ open: boolean }> {
  const res = await fetch("/api/users", { cache: "no-store" });
  return { open: res.status === 200 };
}

function LoginPageInner() {
  const params = useSearchParams();
  const next = params.get("next") || "/";

  const { data, isLoading: checkingSetup } = useSWR("bootstrap", fetchBootstrap);
  const open = data?.open ?? false;

  return (
    <Center h="100vh" bg="var(--mantine-color-body)">
      <Paper withBorder shadow="sm" p="xl" radius="md" w={360}>
        <Title order={2} mb={4}>
          Lorentz
        </Title>
        <Text size="sm" c="dimmed" mb="md">
          {checkingSetup
            ? "Checking the API…"
            : open
              ? "No account exists yet. Create the first admin account."
              : "Sign in to continue."}
        </Text>
        {checkingSetup ? (
          <Center py="lg">
            <Loader size="sm" />
          </Center>
        ) : open ? (
          <BootstrapForm next={next} />
        ) : (
          <LoginForm next={next} />
        )}
      </Paper>
    </Center>
  );
}

export default function LoginPage() {
  // useSearchParams() needs a Suspense boundary in the app router.
  return (
    <Suspense>
      <LoginPageInner />
    </Suspense>
  );
}

async function login(username: string, password: string, totp?: number) {
  const res = await fetch("/api/auth", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      username: username || undefined,
      password,
      ...(totp !== undefined ? { totp } : {}),
    }),
  });
  const body: { session?: Session; error?: { message?: string; key?: string } } = await res.json();
  if (!res.ok || !body?.session?.valid) {
    throw new ApiError(res.status, body?.error?.message ?? "Login failed", body?.error?.key);
  }
  setCsrfToken(body.session.csrf);
  return body.session;
}

function LoginForm({ next }: { next: string }) {
  const router = useRouter();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [totp, setTotp] = useState("");
  const [needsTotp, setNeedsTotp] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  async function onSubmit(e: FormEvent) {
    e.preventDefault();
    setError(null);
    setLoading(true);
    try {
      await login(
        username,
        password,
        needsTotp && totp ? Number(totp) : undefined,
      );
      router.replace(next);
      router.refresh();
    } catch (err) {
      if (err instanceof ApiError) {
        if (/2FA|totp/i.test(err.message)) setNeedsTotp(true);
        setError(err.message);
      } else {
        setError("Login failed");
      }
    } finally {
      setLoading(false);
    }
  }

  return (
    <form onSubmit={onSubmit}>
      <Stack gap="sm">
        <TextInput
          label="Username"
          description="Leave blank to use the configured API password"
          value={username}
          onChange={(e) => setUsername(e.currentTarget.value)}
          autoComplete="username"
          placeholder="(configured password)"
        />
        <PasswordInput
          label="Password"
          value={password}
          onChange={(e) => setPassword(e.currentTarget.value)}
          autoComplete="current-password"
          required
        />
        {needsTotp && (
          <TextInput
            label="2FA code"
            value={totp}
            onChange={(e) => setTotp(e.currentTarget.value)}
            inputMode="numeric"
            autoComplete="one-time-code"
          />
        )}
        {error && (
          <Alert color="red" icon={<IconAlertCircle size={16} />}>
            {error}
          </Alert>
        )}
        <Button type="submit" loading={loading} mt={4}>
          Sign in
        </Button>
      </Stack>
    </form>
  );
}

function BootstrapForm({ next }: { next: string }) {
  const router = useRouter();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [confirm, setConfirm] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  async function onSubmit(e: FormEvent) {
    e.preventDefault();
    setError(null);
    if (password !== confirm) {
      setError("Passwords do not match");
      return;
    }
    setLoading(true);
    try {
      // Unauthenticated while the API is still open (see fetchBootstrap above).
      await api.post("/users", { username, password, role: "admin", enabled: true });
      await login(username, password);
      router.replace(next);
      router.refresh();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not create the account");
    } finally {
      setLoading(false);
    }
  }

  return (
    <form onSubmit={onSubmit}>
      <Stack gap="sm">
        <TextInput
          label="Username"
          value={username}
          onChange={(e) => setUsername(e.currentTarget.value)}
          autoComplete="username"
          required
        />
        <PasswordInput
          label="Password"
          description="8 to 256 characters"
          value={password}
          onChange={(e) => setPassword(e.currentTarget.value)}
          autoComplete="new-password"
          minLength={8}
          required
        />
        <PasswordInput
          label="Confirm password"
          value={confirm}
          onChange={(e) => setConfirm(e.currentTarget.value)}
          autoComplete="new-password"
          required
        />
        {error && (
          <Alert color="red" icon={<IconAlertCircle size={16} />}>
            {error}
          </Alert>
        )}
        <Button type="submit" loading={loading} mt={4}>
          Create admin account
        </Button>
      </Stack>
    </form>
  );
}
