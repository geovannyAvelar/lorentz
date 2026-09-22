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
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
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
    <Stack>
      <Card withBorder padding={0}>
        <Group justify="space-between" p="md">
          <div>
            <Text fw={600}>Lists</Text>
            <Text size="xs" c="dimmed">
              Adlists Lorentz downloads via gravity; run pihole -g (or the equivalent action) after changing these
            </Text>
          </div>
          {isAdmin && (
            <Button size="xs" leftSection={<IconPlus size={14} />} onClick={() => setAdding(true)}>
              Add list
            </Button>
          )}
        </Group>
        {isLoading || !data ? (
          <Center py="xl">
            <Loader />
          </Center>
        ) : data.lists.length === 0 ? (
          <EmptyState title="No lists configured yet" p="xl" />
        ) : (
          <ScrollArea>
            <Table striped highlightOnHover verticalSpacing="xs">
              <Table.Thead>
                <Table.Tr>
                  <Table.Th>Address</Table.Th>
                  <Table.Th>Type</Table.Th>
                  <Table.Th>Domains</Table.Th>
                  <Table.Th>Status</Table.Th>
                  {isAdmin && <Table.Th>Actions</Table.Th>}
                </Table.Tr>
              </Table.Thead>
              <Table.Tbody>
                {data.lists.map((list) => (
                  <Table.Tr key={list.id}>
                    <Table.Td maw={320} style={{ overflow: "hidden", textOverflow: "ellipsis" }}>
                      <Text ff="monospace" size="xs" truncate="end" title={list.address}>
                        {list.address}
                      </Text>
                    </Table.Td>
                    <Table.Td>
                      <Badge color={list.type === "block" ? "red" : "green"}>{list.type}</Badge>
                    </Table.Td>
                    <Table.Td c="dimmed">
                      {typeof list.number === "number" ? list.number.toLocaleString() : "—"}
                    </Table.Td>
                    <Table.Td>
                      <Badge color={list.enabled ? "green" : "gray"}>
                        {list.enabled ? "enabled" : "disabled"}
                      </Badge>
                    </Table.Td>
                    {isAdmin && (
                      <Table.Td>
                        <Group gap="xs" wrap="nowrap">
                          <Button size="xs" variant="default" onClick={() => toggle(list)}>
                            {list.enabled ? "Disable" : "Enable"}
                          </Button>
                          <Button size="xs" variant="default" onClick={() => setEditing(list)}>
                            Edit
                          </Button>
                          <Button size="xs" color="red" variant="light" onClick={() => remove(list)}>
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

      <AddListModal opened={adding} onClose={() => setAdding(false)} onSaved={mutate} />
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
    </Stack>
  );
}

function AddListModal({
  opened,
  onClose,
  onSaved,
}: {
  opened: boolean;
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
    <Modal opened={opened} onClose={onClose} title="Add list">
      <Stack>
        <Textarea
          label="Address"
          description="One URL per line to add several at once"
          value={address}
          onChange={(e) => setAddress(e.currentTarget.value)}
          rows={3}
          placeholder="https://example.com/list.txt"
          required
        />
        <NativeSelect
          label="Type"
          data={[
            { value: "block", label: "Block" },
            { value: "allow", label: "Allow" },
          ]}
          value={type}
          onChange={(e) => setType(e.currentTarget.value as "block" | "allow")}
        />
        <TextInput label="Comment" value={comment} onChange={(e) => setComment(e.currentTarget.value)} />
        <GroupPicker value={groups} onChange={setGroups} />
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
    <Modal opened onClose={onClose} title={list.address}>
      <Stack>
        <NativeSelect
          label="Type"
          data={[
            { value: "block", label: "Block" },
            { value: "allow", label: "Allow" },
          ]}
          value={type}
          onChange={(e) => setType(e.currentTarget.value as "block" | "allow")}
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
            Save
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}
