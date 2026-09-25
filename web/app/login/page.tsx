"use client";

import { Suspense, useState } from "react";
import type { FormEvent } from "react";
import { useRouter, useSearchParams } from "next/navigation";
import useSWR, { useSWRConfig } from "swr";
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
import { SESSION_KEY, fetchSession } from "@/hooks/useSession";
import type { Session } from "@/lib/types";

// Is the API still open, i.e. does no account and no configured password
// exist yet? GET /api/users answers 200 without authentication exactly in
// that state (see users_login_required() in database/user-table.c) and 401
// otherwise, so that single unauthenticated call is enough to tell.
async function fetchBootstrap(): Promise<{ open: boolean }> {
  const res = await fetch("/api/users", { cache: "no-store" });
  return { open: res.status === 200 };
}

// What the pages cache about the session was read before this login; refresh
// it before navigating, or the dashboard would still see a fresh install (a
// session without an id) and send the visitor straight back here. The cached
// "API is open" answer is settled after navigating instead: changing it while
// this page is still up swaps the setup form for the sign-in one mid-login.
function useAfterLogin(next: string) {
  const router = useRouter();
  const { mutate } = useSWRConfig();
  return async () => {
    // Written straight into the cache: nothing on this page is subscribed to
    // it, so a plain revalidation would not fetch anything
    await mutate(SESSION_KEY, await fetchSession(), { revalidate: false });
    router.replace(next);
    router.refresh();
    await mutate("bootstrap", { open: false }, { revalidate: false });
  };
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
              ? "Welcome. Choose a password to get started."
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

async function login(username: string, password: string) {
  const res = await fetch("/api/auth", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ username, password }),
  });
  const body: { session?: Session; error?: { message?: string; key?: string } } = await res.json();
  if (!res.ok || !body?.session?.valid) {
    throw new ApiError(res.status, body?.error?.message ?? "Login failed", body?.error?.key);
  }
  setCsrfToken(body.session.csrf);
  return body.session;
}

function LoginForm({ next }: { next: string }) {
  const afterLogin = useAfterLogin(next);
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  async function onSubmit(e: FormEvent) {
    e.preventDefault();
    setError(null);
    setLoading(true);
    try {
      await login(username.trim(), password);
      await afterLogin();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Login failed");
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
          value={password}
          onChange={(e) => setPassword(e.currentTarget.value)}
          autoComplete="current-password"
          required
        />
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

// A fresh install only asks for a password: the account it belongs to is
// this one, so there is no username to think of. It signs in with it later.
const FIRST_ADMIN = "admin";

function BootstrapForm({ next }: { next: string }) {
  const afterLogin = useAfterLogin(next);
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
      await api.post("/users", { username: FIRST_ADMIN, password, role: "admin", enabled: true });
      await login(FIRST_ADMIN, password);
      await afterLogin();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not set the password");
    } finally {
      setLoading(false);
    }
  }

  return (
    <form onSubmit={onSubmit}>
      <Stack gap="sm">
        {/* For the browser's password manager, which saves it against a name */}
        <input type="hidden" name="username" value={FIRST_ADMIN} autoComplete="username" readOnly />
        <PasswordInput
          label="Password"
          description={`8 to 256 characters. You sign in as "${FIRST_ADMIN}" with it.`}
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
          Set password and continue
        </Button>
      </Stack>
    </form>
  );
}
