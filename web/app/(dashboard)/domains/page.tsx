"use client";

import { useState } from "react";
import useSWR from "swr";
import { api, fetcher, ApiError } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import { Input, Textarea, Select, Checkbox, Labeled } from "@/components/ui/Field";
import { Modal } from "@/components/ui/Modal";
import { ErrorBanner, Spinner, EmptyState } from "@/components/ui/Feedback";
import { GroupPicker } from "@/components/GroupPicker";
import type { DomainEntry, DomainsResponse } from "@/lib/types";
import { cn } from "@/lib/cn";

type DomainType = "allow" | "deny";
type DomainKind = "exact" | "regex";

const TABS: { type: DomainType; kind: DomainKind; label: string }[] = [
  { type: "deny", kind: "exact", label: "Denylist (exact)" },
  { type: "deny", kind: "regex", label: "Denylist (regex)" },
  { type: "allow", kind: "exact", label: "Allowlist (exact)" },
  { type: "allow", kind: "regex", label: "Allowlist (regex)" },
];

export default function DomainsPage() {
  const { isAdmin } = useSession();
  const [tab, setTab] = useState(TABS[0]);
  const [editing, setEditing] = useState<DomainEntry | null>(null);
  const [adding, setAdding] = useState(false);

  const path = `/domains/${tab.type}/${tab.kind}`;
  const { data, isLoading, mutate } = useSWR<DomainsResponse>(path, fetcher);

  async function toggleEnabled(entry: DomainEntry) {
    await api.put(`/domains/${entry.type}/${entry.kind}/${encodeURIComponent(entry.domain)}`, {
      type: entry.type,
      kind: entry.kind,
      comment: entry.comment,
      groups: entry.groups,
      enabled: !entry.enabled,
    });
    mutate();
  }

  async function remove(entry: DomainEntry) {
    if (!confirm(`Delete ${entry.domain} from the ${entry.type} ${entry.kind} list?`)) return;
    await api.del(`/domains/${entry.type}/${entry.kind}/${encodeURIComponent(entry.domain)}`);
    mutate();
  }

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title="Domains"
          actions={isAdmin && <Button size="sm" variant="primary" onClick={() => setAdding(true)}>Add domain</Button>}
        />
        <div className="flex flex-wrap gap-1 border-b border-border px-4 py-2">
          {TABS.map((t) => (
            <button
              key={`${t.type}-${t.kind}`}
              onClick={() => setTab(t)}
              className={cn(
                "rounded-md px-3 py-1.5 text-sm font-medium",
                tab.type === t.type && tab.kind === t.kind
                  ? "bg-accent/15 text-accent"
                  : "text-muted hover:bg-border/50",
              )}
            >
              {t.label}
            </button>
          ))}
        </div>

        {isLoading || !data ? (
          <div className="flex justify-center py-16">
            <Spinner />
          </div>
        ) : data.domains.length === 0 ? (
          <EmptyState title="No entries in this list" />
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-left text-sm">
              <thead className="border-b border-border text-xs uppercase text-muted">
                <tr>
                  <th className="px-4 py-2 font-medium">Domain</th>
                  <th className="px-4 py-2 font-medium">Comment</th>
                  <th className="px-4 py-2 font-medium">Groups</th>
                  <th className="px-4 py-2 font-medium">Status</th>
                  {isAdmin && <th className="px-4 py-2 font-medium">Actions</th>}
                </tr>
              </thead>
              <tbody className="divide-y divide-border">
                {data.domains.map((entry) => (
                  <tr key={entry.id}>
                    <td className="max-w-sm truncate px-4 py-2 font-mono text-xs" title={entry.domain}>
                      {entry.domain}
                    </td>
                    <td className="px-4 py-2 text-muted">{entry.comment || "—"}</td>
                    <td className="px-4 py-2 text-muted">{entry.groups.length}</td>
                    <td className="px-4 py-2">
                      <Badge tone={entry.enabled ? "success" : "neutral"}>
                        {entry.enabled ? "enabled" : "disabled"}
                      </Badge>
                    </td>
                    {isAdmin && (
                      <td className="px-4 py-2">
                        <div className="flex gap-2">
                          <Button size="sm" onClick={() => toggleEnabled(entry)}>
                            {entry.enabled ? "Disable" : "Enable"}
                          </Button>
                          <Button size="sm" onClick={() => setEditing(entry)}>
                            Edit
                          </Button>
                          <Button size="sm" variant="danger" onClick={() => remove(entry)}>
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

      <AddDomainModal
        open={adding}
        onClose={() => setAdding(false)}
        defaultType={tab.type}
        defaultKind={tab.kind}
        onSaved={() => mutate()}
      />
      {editing && (
        <EditDomainModal
          entry={editing}
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

function AddDomainModal({
  open,
  onClose,
  defaultType,
  defaultKind,
  onSaved,
}: {
  open: boolean;
  onClose: () => void;
  defaultType: DomainType;
  defaultKind: DomainKind;
  onSaved: () => void;
}) {
  const [type, setType] = useState<DomainType>(defaultType);
  const [kind, setKind] = useState<DomainKind>(defaultKind);
  const [domain, setDomain] = useState("");
  const [comment, setComment] = useState("");
  const [groups, setGroups] = useState<number[]>([0]);
  const [enabled, setEnabled] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    // One entry per line so several domains can be added at once.
    const domains = domain
      .split("\n")
      .map((d) => d.trim())
      .filter(Boolean);
    if (domains.length === 0) {
      setError("Enter at least one domain");
      return;
    }
    setSaving(true);
    try {
      await api.post(`/domains/${type}/${kind}`, {
        domain: domains.length === 1 ? domains[0] : domains,
        comment: comment || null,
        groups,
        enabled,
      });
      setDomain("");
      setComment("");
      onSaved();
      onClose();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not add the domain");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open={open} onClose={onClose} title="Add domain">
      <div className="flex flex-col gap-3">
        <div className="grid grid-cols-2 gap-3">
          <Labeled label="List">
            <Select value={type} onChange={(e) => setType(e.target.value as DomainType)}>
              <option value="deny">Deny</option>
              <option value="allow">Allow</option>
            </Select>
          </Labeled>
          <Labeled label="Kind">
            <Select value={kind} onChange={(e) => setKind(e.target.value as DomainKind)}>
              <option value="exact">Exact</option>
              <option value="regex">Regex</option>
            </Select>
          </Labeled>
        </div>
        <Labeled label="Domain" hint="One per line to add several at once">
          <Textarea value={domain} onChange={(e) => setDomain(e.target.value)} rows={3} required />
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
            Add
          </Button>
        </div>
      </div>
    </Modal>
  );
}

function EditDomainModal({
  entry,
  onClose,
  onSaved,
}: {
  entry: DomainEntry;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [comment, setComment] = useState(entry.comment ?? "");
  const [groups, setGroups] = useState<number[]>(entry.groups);
  const [enabled, setEnabled] = useState(entry.enabled);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);

  async function submit() {
    setError(null);
    setSaving(true);
    try {
      await api.put(`/domains/${entry.type}/${entry.kind}/${encodeURIComponent(entry.domain)}`, {
        type: entry.type,
        kind: entry.kind,
        comment: comment || null,
        groups,
        enabled,
      });
      onSaved();
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Could not save the domain");
    } finally {
      setSaving(false);
    }
  }

  return (
    <Modal open onClose={onClose} title={entry.domain}>
      <div className="flex flex-col gap-3">
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
