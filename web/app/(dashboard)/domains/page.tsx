"use client";

import { useState } from "react";
import useSWR from "swr";
import {
  Card,
  Group,
  Stack,
  Text,
  Table,
  Tabs,
  Badge,
  Button,
  Modal,
  TextInput,
  Textarea,
  NativeSelect,
  Checkbox,
  Alert,
  Center,
  Loader,
  EmptyState,
  ScrollArea,
} from "@mantine/core";
import { IconPlus, IconAlertCircle } from "@tabler/icons-react";
import { api, fetcher, ApiError } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { GroupPicker } from "@/components/GroupPicker";
import type { DomainEntry, DomainsResponse } from "@/lib/types";

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
    <Stack>
      <Card withBorder padding={0}>
        <Group justify="space-between" p="md">
          <Text fw={600}>Domains</Text>
          {isAdmin && (
            <Button size="xs" leftSection={<IconPlus size={14} />} onClick={() => setAdding(true)}>
              Add domain
            </Button>
          )}
        </Group>
        <Tabs
          value={`${tab.type}-${tab.kind}`}
          onChange={(value) => setTab(TABS.find((t) => `${t.type}-${t.kind}` === value) ?? TABS[0])}
          px="md"
        >
          <Tabs.List>
            {TABS.map((t) => (
              <Tabs.Tab key={`${t.type}-${t.kind}`} value={`${t.type}-${t.kind}`}>
                {t.label}
              </Tabs.Tab>
            ))}
          </Tabs.List>
        </Tabs>

        {isLoading || !data ? (
          <Center py="xl">
            <Loader />
          </Center>
        ) : data.domains.length === 0 ? (
          <EmptyState title="No entries in this list" p="xl" />
        ) : (
          <ScrollArea>
            <Table striped highlightOnHover verticalSpacing="xs">
              <Table.Thead>
                <Table.Tr>
                  <Table.Th>Domain</Table.Th>
                  <Table.Th>Comment</Table.Th>
                  <Table.Th>Groups</Table.Th>
                  <Table.Th>Status</Table.Th>
                  {isAdmin && <Table.Th>Actions</Table.Th>}
                </Table.Tr>
              </Table.Thead>
              <Table.Tbody>
                {data.domains.map((entry) => (
                  <Table.Tr key={entry.id}>
                    <Table.Td maw={320} style={{ overflow: "hidden", textOverflow: "ellipsis" }}>
                      <Text ff="monospace" size="xs" truncate="end" title={entry.domain}>
                        {entry.domain}
                      </Text>
                    </Table.Td>
                    <Table.Td c="dimmed">{entry.comment || "—"}</Table.Td>
                    <Table.Td c="dimmed">{entry.groups.length}</Table.Td>
                    <Table.Td>
                      <Badge color={entry.enabled ? "green" : "gray"}>
                        {entry.enabled ? "enabled" : "disabled"}
                      </Badge>
                    </Table.Td>
                    {isAdmin && (
                      <Table.Td>
                        <Group gap="xs" wrap="nowrap">
                          <Button size="xs" variant="default" onClick={() => toggleEnabled(entry)}>
                            {entry.enabled ? "Disable" : "Enable"}
                          </Button>
                          <Button size="xs" variant="default" onClick={() => setEditing(entry)}>
                            Edit
                          </Button>
                          <Button size="xs" color="red" variant="light" onClick={() => remove(entry)}>
                            Delete
                          </Button>
                        </Group>
                      </Table.Td>
                    )}
                  </Table.Tr>
                ))}
              </Table.Tbody>
            </Table>
          </ScrollArea>
        )}
      </Card>

      <AddDomainModal
        opened={adding}
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
    </Stack>
  );
}

function AddDomainModal({
  opened,
  onClose,
  defaultType,
  defaultKind,
  onSaved,
}: {
  opened: boolean;
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
    <Modal opened={opened} onClose={onClose} title="Add domain">
      <Stack>
        <Group grow>
          <NativeSelect
            label="List"
            data={[
              { value: "deny", label: "Deny" },
              { value: "allow", label: "Allow" },
            ]}
            value={type}
            onChange={(e) => setType(e.currentTarget.value as DomainType)}
          />
          <NativeSelect
            label="Kind"
            data={[
              { value: "exact", label: "Exact" },
              { value: "regex", label: "Regex" },
            ]}
            value={kind}
            onChange={(e) => setKind(e.currentTarget.value as DomainKind)}
          />
        </Group>
        <Textarea
          label="Domain"
          description="One per line to add several at once"
          value={domain}
          onChange={(e) => setDomain(e.currentTarget.value)}
          rows={3}
          required
        />
        <TextInput label="Comment" value={comment} onChange={(e) => setComment(e.currentTarget.value)} />
        <GroupPicker value={groups} onChange={setGroups} />
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
    <Modal opened onClose={onClose} title={entry.domain}>
      <Stack>
        <TextInput label="Comment" value={comment} onChange={(e) => setComment(e.currentTarget.value)} />
        <GroupPicker value={groups} onChange={setGroups} />
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
            Save
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}
