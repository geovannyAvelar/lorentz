"use client";

import { Suspense, useState } from "react";
import type { FormEvent } from "react";
import { useRouter, useSearchParams } from "next/navigation";
import useSWR from "swr";
import { Button } from "@/components/ui/Button";
import { Input, Labeled } from "@/components/ui/Field";
import { ErrorBanner, Spinner } from "@/components/ui/Feedback";
import { Card } from "@/components/ui/Card";
import { ApiError } from "@/lib/client";

async function fetchBootstrap(): Promise<{ open: boolean }> {
  const res = await fetch("/api/bootstrap", { cache: "no-store" });
  return res.json();
}

function LoginPageInner() {
  const params = useSearchParams();
  const next = params.get("next") || "/";

  const { data, isLoading: checkingSetup } = useSWR("bootstrap", fetchBootstrap);
  const open = data?.open ?? false;

  return (
    <div className="flex min-h-dvh items-center justify-center bg-background px-4">
      <Card className="w-full max-w-sm p-6">
        <h1 className="text-lg font-semibold text-foreground">Lorentz</h1>
        <p className="mb-4 mt-1 text-sm text-muted">
          {checkingSetup
            ? "Checking the API…"
            : open
              ? "No account exists yet. Create the first admin account."
              : "Sign in to continue."}
        </p>
        {checkingSetup ? (
          <div className="flex justify-center py-6">
            <Spinner />
          </div>
        ) : open ? (
          <BootstrapForm next={next} />
        ) : (
          <LoginForm next={next} />
        )}
      </Card>
    </div>
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
  const res = await fetch("/api/session", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      username: username || undefined,
      password,
      ...(totp !== undefined ? { totp } : {}),
    }),
  });
  const body = await res.json();
  if (!res.ok || !body?.session?.valid) {
    throw new ApiError(res.status, body?.error?.message ?? "Login failed", body?.error?.key);
  }
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
    <form onSubmit={onSubmit} className="flex flex-col gap-3">
      <Labeled label="Username" hint="Leave blank to use the configured API password">
        <Input
          value={username}
          onChange={(e) => setUsername(e.target.value)}
          autoComplete="username"
          placeholder="(configured password)"
        />
      </Labeled>
      <Labeled label="Password">
        <Input
          type="password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          autoComplete="current-password"
          required
        />
      </Labeled>
      {needsTotp && (
        <Labeled label="2FA code">
          <Input
            value={totp}
            onChange={(e) => setTotp(e.target.value)}
            inputMode="numeric"
            autoComplete="one-time-code"
          />
        </Labeled>
      )}
      <ErrorBanner>{error}</ErrorBanner>
      <Button type="submit" variant="primary" loading={loading} className="mt-1">
        Sign in
      </Button>
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
      const res = await fetch("/api/lorentz/users", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ username, password, role: "admin", enabled: true }),
      });
      const body = await res.json();
      if (!res.ok) {
        throw new ApiError(res.status, body?.error?.message ?? "Could not create the account", body?.error?.key);
      }
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
    <form onSubmit={onSubmit} className="flex flex-col gap-3">
      <Labeled label="Username">
        <Input
          value={username}
          onChange={(e) => setUsername(e.target.value)}
          autoComplete="username"
          required
        />
      </Labeled>
      <Labeled label="Password" hint="8 to 256 characters">
        <Input
          type="password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          autoComplete="new-password"
          minLength={8}
          required
        />
      </Labeled>
      <Labeled label="Confirm password">
        <Input
          type="password"
          value={confirm}
          onChange={(e) => setConfirm(e.target.value)}
          autoComplete="new-password"
          required
        />
      </Labeled>
      <ErrorBanner>{error}</ErrorBanner>
      <Button type="submit" variant="primary" loading={loading} className="mt-1">
        Create admin account
      </Button>
    </form>
  );
}
