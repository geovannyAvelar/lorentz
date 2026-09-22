"use client";

import { useState } from "react";
import useSWR from "swr";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import { Input, Select, Checkbox, Labeled } from "@/components/ui/Field";
import { Modal } from "@/components/ui/Modal";
import { ErrorBanner, Spinner, EmptyState } from "@/components/ui/Feedback";
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
      <Card>
        <EmptyState
          title="Users are managed by an admin"
          description="Your account can change its own password and comment from the Account page."
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
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title="Users"
          description="Accounts that can log in to this API"
          actions={<Button size="sm" variant="primary" onClick={() => setAdding(true)}>Add user</Button>}
        />
        {isLoading || !data ? (
          <div className="flex justify-center py-16">
            {error ? <ErrorBanner>Could not load the users</ErrorBanner> : <Spinner />}
          </div>
        ) : data.users.length === 0 ? (
          <EmptyState title="No accounts yet" />
        ) : (
          <table className="w-full text-left text-sm">
            <thead className="border-b border-border text-xs uppercase text-muted">
              <tr>
                <th className="px-4 py-2 font-medium">Username</th>
                <th className="px-4 py-2 font-medium">Role</th>
                <th className="px-4 py-2 font-medium">Status</th>
                <th className="px-4 py-2 font-medium">Last login</th>
                <th className="px-4 py-2 font-medium">Comment</th>
                <th className="px-4 py-2 font-medium">Actions</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-border">
              {data.users.map((user) => (
                <tr key={user.id}>
                  <td className="px-4 py-2 font-medium">
                    {user.username}
                    {user.username === session?.user?.username && (
                      <span className="ml-1 text-xs text-muted">(you)</span>
                    )}
                  </td>
                  <td className="px-4 py-2">
                    <Badge tone={user.role === "admin" ? "accent" : "neutral"}>{user.role}</Badge>
                  </td>
                  <td className="px-4 py-2">
                    <Badge tone={user.enabled ? "success" : "neutral"}>
                      {user.enabled ? "enabled" : "disabled"}
                    </Badge>
                  </td>
                  <td className="px-4 py-2 text-muted">{timeString(user.last_login)}</td>
                  <td className="px-4 py-2 text-muted">{user.comment || "—"}</td>
                  <td className="px-4 py-2">
                    <div className="flex gap-2">
                      <Button size="sm" onClick={() => setEditing(user)}>
                        Edit
                      </Button>
                      <Button
                        size="sm"
                        variant="danger"
                        onClick={() => remove(user)}
                        disabled={user.username === session?.user?.username}
                      >
                        Delete
                      </Button>
                    </div>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Card>

      <AddUserModal open={adding} onClose={() => setAdding(false)} onSaved={mutate} />
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
    </div>
  );
}

function AddUserModal({
  open,
  onClose,
  onSaved,
}: {
  open: boolean;
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
    <Modal open={open} onClose={onClose} title="Add user">
      <div className="flex flex-col gap-3">
        <Labeled label="Username" hint="1-64 letters, digits and . _ - @">
          <Input value={username} onChange={(e) => setUsername(e.target.value)} required />
        </Labeled>
        <Labeled label="Password" hint="8 to 256 characters">
          <Input
            type="password"
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            minLength={8}
            required
          />
        </Labeled>
        <Labeled label="Role">
          <Select value={role} onChange={(e) => setRole(e.target.value as "admin" | "viewer")}>
            <option value="viewer">Viewer</option>
            <option value="admin">Admin</option>
          </Select>
        </Labeled>
        <Labeled label="Comment">
          <Input value={comment} onChange={(e) => setComment(e.target.value)} />
        </Labeled>
        <Checkbox checked={enabled} onChange={(e) => setEnabled(e.target.checked)} label="Enabled" />
        <ErrorBanner>{error}</ErrorBanner>
        <div className="flex justify-end gap-2">
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" loading={saving} onClick={submit}>
            Add
          </Button>
        </div>
      </div>
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
    <Modal open onClose={onClose} title={user.username}>
      <div className="flex flex-col gap-3">
        {isSelf && (
          <p className="text-xs text-muted">
            You cannot change your own role or disable your own account.
          </p>
        )}
        <Labeled label="Role">
          <Select
            value={role}
            onChange={(e) => setRole(e.target.value as "admin" | "viewer")}
            disabled={isSelf}
          >
            <option value="viewer">Viewer</option>
            <option value="admin">Admin</option>
          </Select>
        </Labeled>
        <Checkbox
          checked={enabled}
          onChange={(e) => setEnabled(e.target.checked)}
          label="Enabled"
          disabled={isSelf}
        />
        <Labeled label="Comment">
          <Input value={comment} onChange={(e) => setComment(e.target.value)} />
        </Labeled>
        {isSelf ? (
          <p className="text-xs text-muted">
            Change your own password from the Account page.
          </p>
        ) : (
          <Labeled label="New password" hint="Leave blank to keep the current password. Resetting it ends the account's other sessions.">
            <Input
              type="password"
              value={newPassword}
              onChange={(e) => setNewPassword(e.target.value)}
              minLength={8}
            />
          </Labeled>
        )}
        <ErrorBanner>{error}</ErrorBanner>
        <div className="flex justify-end gap-2">
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" loading={saving} onClick={submit}>
            Save
          </Button>
        </div>
      </div>
    </Modal>
  );
}
