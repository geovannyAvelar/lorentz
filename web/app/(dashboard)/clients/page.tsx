"use client";

import { useState } from "react";
import useSWR from "swr";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Input, Textarea, Labeled } from "@/components/ui/Field";
import { Modal } from "@/components/ui/Modal";
import { ErrorBanner, Spinner, EmptyState } from "@/components/ui/Feedback";
import { GroupPicker } from "@/components/GroupPicker";
import type { Client, ClientsResponse } from "@/lib/types";

export default function ClientsPage() {
  const { isAdmin } = useSession();
  const { data, isLoading, mutate } = useSWR<ClientsResponse>("/clients", fetcher);
  const [adding, setAdding] = useState(false);
  const [editing, setEditing] = useState<Client | null>(null);

  async function remove(client: Client) {
    if (!confirm(`Remove client ${client.client} from its groups?`)) return;
    await api.del(`/clients/${encodeURIComponent(client.client)}`);
    mutate();
  }

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title="Clients"
          description="Clients configured here get non-default group assignments"
          actions={isAdmin && <Button size="sm" variant="primary" onClick={() => setAdding(true)}>Add client</Button>}
        />
        {isLoading || !data ? (
          <div className="flex justify-center py-16">
            <Spinner />
          </div>
        ) : data.clients.length === 0 ? (
          <EmptyState title="No clients configured yet" description="Every client uses the default group until it is added here" />
        ) : (
          <table className="w-full text-left text-sm">
            <thead className="border-b border-border text-xs uppercase text-muted">
              <tr>
                <th className="px-4 py-2 font-medium">Client</th>
                <th className="px-4 py-2 font-medium">Comment</th>
                <th className="px-4 py-2 font-medium">Groups</th>
                {isAdmin && <th className="px-4 py-2 font-medium">Actions</th>}
              </tr>
            </thead>
            <tbody className="divide-y divide-border">
              {data.clients.map((client) => (
                <tr key={client.id}>
                  <td className="px-4 py-2 font-mono text-xs">{client.client}</td>
                  <td className="px-4 py-2 text-muted">{client.comment || "—"}</td>
                  <td className="px-4 py-2 text-muted">{client.groups.length}</td>
                  {isAdmin && (
                    <td className="px-4 py-2">
                      <div className="flex gap-2">
                        <Button size="sm" onClick={() => setEditing(client)}>
                          Edit
                        </Button>
                        <Button size="sm" variant="danger" onClick={() => remove(client)}>
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

      <AddClientModal open={adding} onClose={() => setAdding(false)} onSaved={mutate} />
      {editing && (
        <EditClientModal
          client={editing}
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

function AddClientModal({
  open,
  onClose,
  onSaved,
}: {
  open: boolean;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [client, setClient] = useState("");
  const [comment, setComment] = useState("");
  const [groups, setGroups] = useState<number[]>([0]);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    const clients = client
      .split("\n")
      .map((c) => c.trim())
      .filter(Boolean);
    if (clients.length === 0) {
      setError("Enter at least one client");
      return;
    }
    setError(null);
    setSaving(true);
    try {
      await api.post("/clients", {
        client: clients.length === 1 ? clients[0] : clients,
        comment: comment || null,
        groups,
      });
      setClient("");
      setComment("");
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not add the client");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open={open} onClose={onClose} title="Add client">
      <div className="flex flex-col gap-3">
        <Labeled label="Client" hint="IP, MAC, hostname or interface (name:eth0). One per line for several at once">
          <Textarea value={client} onChange={(e) => setClient(e.target.value)} rows={3} required />
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

function EditClientModal({
  client,
  onClose,
  onSaved,
}: {
  client: Client;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [comment, setComment] = useState(client.comment ?? "");
  const [groups, setGroups] = useState<number[]>(client.groups);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    setSaving(true);
    try {
      await api.put(`/clients/${encodeURIComponent(client.client)}`, {
        comment: comment || null,
        groups,
      });
      onSaved();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not save the client");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open onClose={onClose} title={client.client}>
      <div className="flex flex-col gap-3">
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
            Save
          </Button>
        </div>
      </div>
    </Modal>
  );
}
