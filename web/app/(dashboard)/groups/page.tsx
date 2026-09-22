"use client";

import { useState } from "react";
import useSWR from "swr";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import { Input, Checkbox, Labeled } from "@/components/ui/Field";
import { Modal } from "@/components/ui/Modal";
import { ErrorBanner, Spinner, EmptyState } from "@/components/ui/Feedback";
import type { Group, GroupsResponse } from "@/lib/types";

export default function GroupsPage() {
  const { isAdmin } = useSession();
  const { data, isLoading, mutate } = useSWR<GroupsResponse>("/groups", fetcher);
  const [adding, setAdding] = useState(false);
  const [editing, setEditing] = useState<Group | null>(null);

  async function toggle(group: Group) {
    await api.put(`/groups/${encodeURIComponent(group.name)}`, {
      name: group.name,
      comment: group.comment,
      enabled: !group.enabled,
    });
    mutate();
  }

  async function remove(group: Group) {
    if (!confirm(`Delete group "${group.name}"? Domains, lists and clients keep their other groups.`)) return;
    await api.del(`/groups/${encodeURIComponent(group.name)}`);
    mutate();
  }

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title="Groups"
          description="Group 0 is the default group, applied to every client that has no group of its own"
          actions={isAdmin && <Button size="sm" variant="primary" onClick={() => setAdding(true)}>Add group</Button>}
        />
        {isLoading || !data ? (
          <div className="flex justify-center py-16">
            <Spinner />
          </div>
        ) : data.groups.length === 0 ? (
          <EmptyState title="No groups defined yet" />
        ) : (
          <table className="w-full text-left text-sm">
            <thead className="border-b border-border text-xs uppercase text-muted">
              <tr>
                <th className="px-4 py-2 font-medium">Name</th>
                <th className="px-4 py-2 font-medium">Comment</th>
                <th className="px-4 py-2 font-medium">Status</th>
                {isAdmin && <th className="px-4 py-2 font-medium">Actions</th>}
              </tr>
            </thead>
            <tbody className="divide-y divide-border">
              {data.groups.map((group) => (
                <tr key={group.name}>
                  <td className="px-4 py-2 font-medium">{group.name}</td>
                  <td className="px-4 py-2 text-muted">{group.comment || "—"}</td>
                  <td className="px-4 py-2">
                    <Badge tone={group.enabled ? "success" : "neutral"}>
                      {group.enabled ? "enabled" : "disabled"}
                    </Badge>
                  </td>
                  {isAdmin && (
                    <td className="px-4 py-2">
                      <div className="flex gap-2">
                        <Button size="sm" onClick={() => toggle(group)}>
                          {group.enabled ? "Disable" : "Enable"}
                        </Button>
                        <Button size="sm" onClick={() => setEditing(group)}>
                          Edit
                        </Button>
                        <Button size="sm" variant="danger" onClick={() => remove(group)}>
                          Delete
                        </Button>
                      </div>
                    </td>
                  )}
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Card>

      <AddGroupModal open={adding} onClose={() => setAdding(false)} onSaved={mutate} />
      {editing && (
        <EditGroupModal
          group={editing}
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

function AddGroupModal({
  open,
  onClose,
  onSaved,
}: {
  open: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [name, setName] = useState("");
  const [comment, setComment] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    if (!name.trim()) {
      setError("Enter a name");
      return;
    }
    setError(null);
    setSaving(true);
    try {
      await api.post("/groups", { name: name.trim(), comment: comment || null, enabled: true });
      setName("");
      setComment("");
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not create the group");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open={open} onClose={onClose} title="Add group">
      <div className="flex flex-col gap-3">
        <Labeled label="Name">
          <Input value={name} onChange={(e) => setName(e.target.value)} required />
        </Labeled>
        <Labeled label="Comment">
          <Input value={comment} onChange={(e) => setComment(e.target.value)} />
        </Labeled>
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

function EditGroupModal({
  group,
  onClose,
  onSaved,
}: {
  group: Group;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [name, setName] = useState(group.name);
  const [comment, setComment] = useState(group.comment ?? "");
  const [enabled, setEnabled] = useState(group.enabled);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    setSaving(true);
    try {
      await api.put(`/groups/${encodeURIComponent(group.name)}`, {
        name: name.trim(),
        comment: comment || null,
        enabled,
      });
      onSaved();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not save the group");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open onClose={onClose} title={group.name}>
      <div className="flex flex-col gap-3">
        <Labeled label="Name">
          <Input value={name} onChange={(e) => setName(e.target.value)} disabled={group.name === "Default"} />
        </Labeled>
        <Labeled label="Comment">
          <Input value={comment} onChange={(e) => setComment(e.target.value)} />
        </Labeled>
        <Checkbox checked={enabled} onChange={(e) => setEnabled(e.target.checked)} label="Enabled" />
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
