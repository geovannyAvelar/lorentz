"use client";

import { useState } from "react";
import useSWR from "swr";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import { Input, Labeled } from "@/components/ui/Field";
import { ErrorBanner, Spinner } from "@/components/ui/Feedback";
import type { UsersResponse } from "@/lib/types";

export default function AccountPage() {
  const { session } = useSession();

  if (!session?.user) {
    return (
      <Card>
        <CardHeader title="Account" />
        <p className="px-4 py-6 text-sm text-muted">
          You are signed in with the password of the configuration
          (webserver.api.password), not with an account. There is nothing to
          manage here - create an account under Users to get a self-service
          profile with its own password.
        </p>
      </Card>
    );
  }

  return <AccountDetails username={session.user.username} />;
}

function AccountDetails({ username }: { username: string }) {
  const { data, mutate } = useSWR<UsersResponse>(`/users/${encodeURIComponent(username)}`, fetcher);
  const user = data?.users[0];

  const [comment, setComment] = useState<string | null>(null);
  const [currentPassword, setCurrentPassword] = useState("");
  const [newPassword, setNewPassword] = useState("");
  const [confirmPassword, setConfirmPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [success, setSuccess] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  const commentValue = comment ?? user?.comment ?? "";

  async function saveComment() {
    setError(null);
    setSuccess(null);
    setSaving(true);
    try {
      await api.put(`/users/${encodeURIComponent(username)}`, { comment: commentValue || null });
      setSuccess("Comment saved");
      mutate();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not save the comment");
    } finally {
      setSaving(false);
    }
  }

  async function changePassword() {
    setError(null);
    setSuccess(null);
    if (newPassword !== confirmPassword) {
      setError("New passwords do not match");
      return;
    }
    setSaving(true);
    try {
      await api.put(`/users/${encodeURIComponent(username)}`, {
        password: newPassword,
        current_password: currentPassword,
      });
      setCurrentPassword("");
      setNewPassword("");
      setConfirmPassword("");
      setSuccess("Password changed. Your other sessions have been signed out.");
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not change the password");
    } finally {
      setSaving(false);
    }
  }

  if (!user) {
    return (
      <div className="flex justify-center py-16">
        <Spinner />
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title={user.username}
          description={`Account · ${new Date(user.created_at * 1000).toLocaleDateString()}`}
          actions={<Badge tone={user.role === "admin" ? "accent" : "neutral"}>{user.role}</Badge>}
        />
        <div className="flex flex-col gap-3 p-4">
          <Labeled label="Comment">
            <Input value={commentValue} onChange={(e) => setComment(e.target.value)} />
          </Labeled>
          <div>
            <Button size="sm" variant="primary" loading={saving} onClick={saveComment}>
              Save comment
            </Button>
          </div>
        </div>
      </Card>

      <Card>
        <CardHeader title="Change password" />
        <div className="flex max-w-sm flex-col gap-3 p-4">
          <Labeled label="Current password">
            <Input
              type="password"
              value={currentPassword}
              onChange={(e) => setCurrentPassword(e.target.value)}
              autoComplete="current-password"
            />
          </Labeled>
          <Labeled label="New password" hint="8 to 256 characters">
            <Input
              type="password"
              value={newPassword}
              onChange={(e) => setNewPassword(e.target.value)}
              autoComplete="new-password"
              minLength={8}
            />
          </Labeled>
          <Labeled label="Confirm new password">
            <Input
              type="password"
              value={confirmPassword}
              onChange={(e) => setConfirmPassword(e.target.value)}
              autoComplete="new-password"
            />
          </Labeled>
          {error && <ErrorBanner>{error}</ErrorBanner>}
          {success && (
            <p className="rounded-md border border-success/30 bg-success/10 px-3 py-2 text-sm text-success">
              {success}
            </p>
          )}
          <div>
            <Button
              variant="primary"
              loading={saving}
              onClick={changePassword}
              disabled={!currentPassword || !newPassword}
            >
              Change password
            </Button>
          </div>
        </div>
      </Card>
    </div>
  );
}
