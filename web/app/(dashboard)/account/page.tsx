"use client";

import { useState } from "react";
import useSWR from "swr";
import {
  Card,
  Group,
  Stack,
  Text,
  TextInput,
  PasswordInput,
  Button,
  Badge,
  Alert,
  Center,
  Loader,
} from "@mantine/core";
import { IconAlertCircle, IconCheck } from "@tabler/icons-react";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import type { UsersResponse } from "@/lib/types";

export default function AccountPage() {
  const { session } = useSession();

  if (!session?.user) {
    return (
      <Card withBorder>
        <Text fw={600} mb="xs">Account</Text>
        <Text size="sm" c="dimmed">
          You are signed in with the password of the configuration
          (webserver.api.password), not with an account. There is nothing to
          manage here - create an account under Users to get a self-service
          profile with its own password.
        </Text>
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
      <Center py="xl">
        <Loader />
      </Center>
    );
  }

  return (
    <Stack>
      <Card withBorder>
        <Group justify="space-between" mb="md">
          <div>
            <Text fw={600}>{user.username}</Text>
            <Text size="xs" c="dimmed">
              Account · {new Date(user.created_at * 1000).toLocaleDateString()}
            </Text>
          </div>
          <Badge color={user.role === "admin" ? "blue" : "gray"}>{user.role}</Badge>
        </Group>
        <Stack gap="sm" maw={420}>
          <TextInput label="Comment" value={commentValue} onChange={(e) => setComment(e.currentTarget.value)} />
          <Group>
            <Button size="sm" loading={saving} onClick={saveComment}>
              Save comment
            </Button>
          </Group>
        </Stack>
      </Card>

      <Card withBorder>
        <Text fw={600} mb="md">Change password</Text>
        <Stack gap="sm" maw={360}>
          <PasswordInput
            label="Current password"
            value={currentPassword}
            onChange={(e) => setCurrentPassword(e.currentTarget.value)}
            autoComplete="current-password"
          />
          <PasswordInput
            label="New password"
            description="8 to 256 characters"
            value={newPassword}
            onChange={(e) => setNewPassword(e.currentTarget.value)}
            autoComplete="new-password"
            minLength={8}
          />
          <PasswordInput
            label="Confirm new password"
            value={confirmPassword}
            onChange={(e) => setConfirmPassword(e.currentTarget.value)}
            autoComplete="new-password"
          />
          {error && (
            <Alert color="red" icon={<IconAlertCircle size={16} />}>
              {error}
            </Alert>
          )}
          {success && (
            <Alert color="green" icon={<IconCheck size={16} />}>
              {success}
            </Alert>
          )}
          <Group>
            <Button
              loading={saving}
              onClick={changePassword}
              disabled={!currentPassword || !newPassword}
            >
              Change password
            </Button>
          </Group>
        </Stack>
      </Card>
    </Stack>
  );
}
