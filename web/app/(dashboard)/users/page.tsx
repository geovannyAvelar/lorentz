"use client";

import { useState } from "react";
import useSWR from "swr";
import {
  Card,
  Group,
  Stack,
  Text,
  Table,
  Badge,
  Button,
  Modal,
  TextInput,
  PasswordInput,
  NativeSelect,
  Checkbox,
  Alert,
  Center,
  Loader,
  EmptyState,
} from "@mantine/core";
import { IconPlus, IconAlertCircle } from "@tabler/icons-react";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import type { User, UsersResponse } from "@/lib/types";

function timeString(unixSeconds: number | null) {
  return unixSeconds ? new Date(unixSeconds * 1000).toLocaleString() : "never";
}

export default function UsersPage() {
  const { isAdmin, session } = useSession();
  const { data, isLoading, mutate, error } = useSWR<UsersResponse>(
    isAdmin ? "/users" : null,
    fetcher,
  );
  const [adding, setAdding] = useState(false);
  const [editing, setEditing] = useState<User | null>(null);

  if (!isAdmin) {
    return (
      <Card withBorder>
        <EmptyState
          title="Users are managed by an admin"
          description="Your account can change its own password and comment from the Account page."
          p="xl"
        />
      </Card>
    );
  }

  async function remove(user: User) {
    if (!confirm(`Delete the account "${user.username}"?`)) return;
    try {
      await api.del(`/users/${encodeURIComponent(user.username)}`);
      mutate();
    } catch (err) {
      alert(err instanceof ApiError ? err.message : "Could not delete the account");
    }
  }

  return (
    <Stack>
      <Card withBorder padding={0}>
        <Group justify="space-between" p="md">
          <div>
            <Text fw={600}>Users</Text>
            <Text size="xs" c="dimmed">Accounts that can log in to this API</Text>
          </div>
          <Button size="xs" leftSection={<IconPlus size={14} />} onClick={() => setAdding(true)}>
            Add user
          </Button>
        </Group>
        {isLoading || !data ? (
          <Center py="xl">
            {error ? <Alert color="red">Could not load the users</Alert> : <Loader />}
          </Center>
        ) : data.users.length === 0 ? (
          <EmptyState title="No accounts yet" p="xl" />
        ) : (
          <Table striped highlightOnHover verticalSpacing="xs">
            <Table.Thead>
              <Table.Tr>
                <Table.Th>Username</Table.Th>
                <Table.Th>Role</Table.Th>
                <Table.Th>Status</Table.Th>
                <Table.Th>Last login</Table.Th>
                <Table.Th>Comment</Table.Th>
                <Table.Th>Actions</Table.Th>
              </Table.Tr>
            </Table.Thead>
            <Table.Tbody>
              {data.users.map((user) => (
                <Table.Tr key={user.id}>
                  <Table.Td fw={500}>
                    {user.username}
                    {user.username === session?.user?.username && (
                      <Text span size="xs" c="dimmed"> (you)</Text>
                    )}
                  </Table.Td>
                  <Table.Td>
                    <Badge color={user.role === "admin" ? "blue" : "gray"}>{user.role}</Badge>
                  </Table.Td>
                  <Table.Td>
                    <Badge color={user.enabled ? "green" : "gray"}>
                      {user.enabled ? "enabled" : "disabled"}
                    </Badge>
                  </Table.Td>
                  <Table.Td c="dimmed">{timeString(user.last_login)}</Table.Td>
                  <Table.Td c="dimmed">{user.comment || "—"}</Table.Td>
                  <Table.Td>
                    <Group gap="xs" wrap="nowrap">
                      <Button size="xs" variant="default" onClick={() => setEditing(user)}>
                        Edit
                      </Button>
                      <Button
                        size="xs"
                        color="red"
                        variant="light"
                        onClick={() => remove(user)}
                        disabled={user.username === session?.user?.username}
                      >
                        Delete
                      </Button>
                    </Group>
                  </Table.Td>
                </Table.Tr>
              ))}
            </Table.Tbody>
          </Table>
        )}
      </Card>

      <AddUserModal opened={adding} onClose={() => setAdding(false)} onSaved={mutate} />
      {editing && (
        <EditUserModal
          user={editing}
          isSelf={editing.username === session?.user?.username}
          onClose={() => setEditing(null)}
          onSaved={() => {
            setEditing(null);
            mutate();
          }}
        />
      )}
    </Stack>
  );
}

function AddUserModal({
  opened,
  onClose,
  onSaved,
}: {
  opened: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [role, setRole] = useState<"admin" | "viewer">("viewer");
  const [enabled, setEnabled] = useState(true);
  const [comment, setComment] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    setSaving(true);
    try {
      await api.post("/users", { username, password, role, enabled, comment: comment || null });
      setUsername("");
      setPassword("");
      setComment("");
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not create the account");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal opened={opened} onClose={onClose} title="Add user">
      <Stack>
        <TextInput
          label="Username"
          description="1-64 letters, digits and . _ - @"
          value={username}
          onChange={(e) => setUsername(e.currentTarget.value)}
          required
        />
        <PasswordInput
          label="Password"
          description="8 to 256 characters"
          value={password}
          onChange={(e) => setPassword(e.currentTarget.value)}
          minLength={8}
          required
        />
        <NativeSelect
          label="Role"
          data={[
            { value: "viewer", label: "Viewer" },
            { value: "admin", label: "Admin" },
          ]}
          value={role}
          onChange={(e) => setRole(e.currentTarget.value as "admin" | "viewer")}
        />
        <TextInput label="Comment" value={comment} onChange={(e) => setComment(e.currentTarget.value)} />
        <Checkbox checked={enabled} onChange={(e) => setEnabled(e.currentTarget.checked)} label="Enabled" />
        {error && (
          <Alert color="red" icon={<IconAlertCircle size={16} />}>
            {error}
          </Alert>
        )}
        <Group justify="flex-end">
          <Button variant="default" onClick={onClose}>
            Cancel
          </Button>
          <Button loading={saving} onClick={submit}>
            Add
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}

function EditUserModal({
  user,
  isSelf,
  onClose,
  onSaved,
}: {
  user: User;
  isSelf: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [role, setRole] = useState(user.role);
  const [enabled, setEnabled] = useState(user.enabled);
  const [comment, setComment] = useState(user.comment ?? "");
  const [newPassword, setNewPassword] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    const changes: Record<string, unknown> = { comment: comment || null };
    if (!isSelf) {
      changes.role = role;
      changes.enabled = enabled;
      if (newPassword) changes.password = newPassword;
    }
    setSaving(true);
    try {
      await api.put(`/users/${encodeURIComponent(user.username)}`, changes);
      onSaved();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not save the account");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal opened onClose={onClose} title={user.username}>
      <Stack>
        {isSelf && (
          <Text size="xs" c="dimmed">
            You cannot change your own role or disable your own account.
          </Text>
        )}
        <NativeSelect
          label="Role"
          data={[
            { value: "viewer", label: "Viewer" },
            { value: "admin", label: "Admin" },
          ]}
          value={role}
          onChange={(e) => setRole(e.currentTarget.value as "admin" | "viewer")}
          disabled={isSelf}
        />
        <Checkbox
          checked={enabled}
          onChange={(e) => setEnabled(e.currentTarget.checked)}
          label="Enabled"
          disabled={isSelf}
        />
        <TextInput label="Comment" value={comment} onChange={(e) => setComment(e.currentTarget.value)} />
        {isSelf ? (
          <Text size="xs" c="dimmed">
            Change your own password from the Account page.
          </Text>
        ) : (
          <PasswordInput
            label="New password"
            description="Leave blank to keep the current password. Resetting it ends the account's other sessions."
            value={newPassword}
            onChange={(e) => setNewPassword(e.currentTarget.value)}
            minLength={8}
          />
        )}
        {error && (
          <Alert color="red" icon={<IconAlertCircle size={16} />}>
            {error}
          </Alert>
        )}
        <Group justify="flex-end">
          <Button variant="default" onClick={onClose}>
            Cancel
          </Button>
          <Button loading={saving} onClick={submit}>
            Save
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}
