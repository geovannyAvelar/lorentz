"use client";

import { useState } from "react";
import useSWR from "swr";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import { Input, Textarea, Select, Checkbox, Labeled } from "@/components/ui/Field";
import { Modal } from "@/components/ui/Modal";
import { ErrorBanner, Spinner, EmptyState } from "@/components/ui/Feedback";
import { GroupPicker } from "@/components/GroupPicker";
import type { AdList, ListsResponse } from "@/lib/types";

export default function ListsPage() {
  const { isAdmin } = useSession();
  const { data, isLoading, mutate } = useSWR<ListsResponse>("/lists", fetcher);
  const [adding, setAdding] = useState(false);
  const [editing, setEditing] = useState<AdList | null>(null);

  async function toggle(list: AdList) {
    await api.put(`/lists/${encodeURIComponent(list.address)}`, {
      type: list.type,
      comment: list.comment,
      groups: list.groups,
      enabled: !list.enabled,
    });
    mutate();
  }

  async function remove(list: AdList) {
    if (!confirm(`Remove list ${list.address}?`)) return;
    await api.del(`/lists/${encodeURIComponent(list.address)}`);
    mutate();
  }

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title="Lists"
          description="Adlists Lorentz downloads via gravity; run pihole -g (or the equivalent action) after changing these"
          actions={isAdmin && <Button size="sm" variant="primary" onClick={() => setAdding(true)}>Add list</Button>}
        />
        {isLoading || !data ? (
          <div className="flex justify-center py-16">
            <Spinner />
          </div>
        ) : data.lists.length === 0 ? (
          <EmptyState title="No lists configured yet" />
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-left text-sm">
              <thead className="border-b border-border text-xs uppercase text-muted">
                <tr>
                  <th className="px-4 py-2 font-medium">Address</th>
                  <th className="px-4 py-2 font-medium">Type</th>
                  <th className="px-4 py-2 font-medium">Domains</th>
                  <th className="px-4 py-2 font-medium">Status</th>
                  {isAdmin && <th className="px-4 py-2 font-medium">Actions</th>}
                </tr>
              </thead>
              <tbody className="divide-y divide-border">
                {data.lists.map((list) => (
                  <tr key={list.id}>
                    <td className="max-w-sm truncate px-4 py-2 font-mono text-xs" title={list.address}>
                      {list.address}
                    </td>
                    <td className="px-4 py-2">
                      <Badge tone={list.type === "block" ? "danger" : "success"}>{list.type}</Badge>
                    </td>
                    <td className="px-4 py-2 text-muted">
                      {typeof list.number === "number" ? list.number.toLocaleString() : "—"}
                    </td>
                    <td className="px-4 py-2">
                      <Badge tone={list.enabled ? "success" : "neutral"}>
                        {list.enabled ? "enabled" : "disabled"}
                      </Badge>
                    </td>
                    {isAdmin && (
                      <td className="px-4 py-2">
                        <div className="flex gap-2">
                          <Button size="sm" onClick={() => toggle(list)}>
                            {list.enabled ? "Disable" : "Enable"}
                          </Button>
                          <Button size="sm" onClick={() => setEditing(list)}>
                            Edit
                          </Button>
                          <Button size="sm" variant="danger" onClick={() => remove(list)}>
                            Delete
                          </Button>
                        </div>
                      </td>
                    )}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>

      <AddListModal open={adding} onClose={() => setAdding(false)} onSaved={mutate} />
      {editing && (
        <EditListModal
          list={editing}
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

function AddListModal({
  open,
  onClose,
  onSaved,
}: {
  open: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [address, setAddress] = useState("");
  const [type, setType] = useState<"block" | "allow">("block");
  const [comment, setComment] = useState("");
  const [groups, setGroups] = useState<number[]>([0]);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    const addresses = address
      .split("\n")
      .map((a) => a.trim())
      .filter(Boolean);
    if (addresses.length === 0) {
      setError("Enter at least one address");
      return;
    }
    setError(null);
    setSaving(true);
    try {
      await api.post("/lists", {
        address: addresses.length === 1 ? addresses[0] : addresses,
        type,
        comment: comment || null,
        groups,
        enabled: true,
      });
      setAddress("");
      setComment("");
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not add the list");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open={open} onClose={onClose} title="Add list">
      <div className="flex flex-col gap-3">
        <Labeled label="Address" hint="One URL per line to add several at once">
          <Textarea
            value={address}
            onChange={(e) => setAddress(e.target.value)}
            rows={3}
            placeholder="https://example.com/list.txt"
            required
          />
        </Labeled>
        <Labeled label="Type">
          <Select value={type} onChange={(e) => setType(e.target.value as "block" | "allow")}>
            <option value="block">Block</option>
            <option value="allow">Allow</option>
          </Select>
        </Labeled>
        <Labeled label="Comment">
          <Input value={comment} onChange={(e) => setComment(e.target.value)} />
        </Labeled>
        <Labeled label="Groups">
          <GroupPicker value={groups} onChange={setGroups} />
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

function EditListModal({
  list,
  onClose,
  onSaved,
}: {
  list: AdList;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [type, setType] = useState(list.type);
  const [comment, setComment] = useState(list.comment ?? "");
  const [groups, setGroups] = useState<number[]>(list.groups);
  const [enabled, setEnabled] = useState(list.enabled);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    setSaving(true);
    try {
      await api.put(`/lists/${encodeURIComponent(list.address)}`, {
        type,
        comment: comment || null,
        groups,
        enabled,
      });
      onSaved();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not save the list");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open onClose={onClose} title={list.address}>
      <div className="flex flex-col gap-3">
        <Labeled label="Type">
          <Select value={type} onChange={(e) => setType(e.target.value as "block" | "allow")}>
            <option value="block">Block</option>
            <option value="allow">Allow</option>
          </Select>
        </Labeled>
        <Labeled label="Comment">
          <Input value={comment} onChange={(e) => setComment(e.target.value)} />
        </Labeled>
        <Labeled label="Groups">
          <GroupPicker value={groups} onChange={setGroups} />
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
