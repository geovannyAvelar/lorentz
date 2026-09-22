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
  Checkbox,
  Alert,
  Center,
  Loader,
  EmptyState,
} from "@mantine/core";
import { IconPlus, IconAlertCircle } from "@tabler/icons-react";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import type { Group as GroupEntry, GroupsResponse } from "@/lib/types";

export default function GroupsPage() {
  const { isAdmin } = useSession();
  const { data, isLoading, mutate } = useSWR<GroupsResponse>("/groups", fetcher);
  const [adding, setAdding] = useState(false);
  const [editing, setEditing] = useState<GroupEntry | null>(null);

  async function toggle(group: GroupEntry) {
    await api.put(`/groups/${encodeURIComponent(group.name)}`, {
      name: group.name,
      comment: group.comment,
      enabled: !group.enabled,
    });
    mutate();
  }

  async function remove(group: GroupEntry) {
    if (!confirm(`Delete group "${group.name}"? Domains, lists and clients keep their other groups.`)) return;
    await api.del(`/groups/${encodeURIComponent(group.name)}`);
    mutate();
  }

  return (
    <Stack>
      <Card withBorder padding={0}>
        <Group justify="space-between" p="md">
          <div>
            <Text fw={600}>Groups</Text>
            <Text size="xs" c="dimmed">
              Group 0 is the default group, applied to every client that has no group of its own
            </Text>
          </div>
          {isAdmin && (
            <Button size="xs" leftSection={<IconPlus size={14} />} onClick={() => setAdding(true)}>
              Add group
            </Button>
          )}
        </Group>
        {isLoading || !data ? (
          <Center py="xl">
            <Loader />
          </Center>
        ) : data.groups.length === 0 ? (
          <EmptyState title="No groups defined yet" p="xl" />
        ) : (
          <Table striped highlightOnHover verticalSpacing="xs">
            <Table.Thead>
              <Table.Tr>
                <Table.Th>Name</Table.Th>
                <Table.Th>Comment</Table.Th>
                <Table.Th>Status</Table.Th>
                {isAdmin && <Table.Th>Actions</Table.Th>}
              </Table.Tr>
            </Table.Thead>
            <Table.Tbody>
              {data.groups.map((group) => (
                <Table.Tr key={group.name}>
                  <Table.Td fw={500}>{group.name}</Table.Td>
                  <Table.Td c="dimmed">{group.comment || "—"}</Table.Td>
                  <Table.Td>
                    <Badge color={group.enabled ? "green" : "gray"}>
                      {group.enabled ? "enabled" : "disabled"}
                    </Badge>
                  </Table.Td>
                  {isAdmin && (
                    <Table.Td>
                      <Group gap="xs" wrap="nowrap">
                        <Button size="xs" variant="default" onClick={() => toggle(group)}>
                          {group.enabled ? "Disable" : "Enable"}
                        </Button>
                        <Button size="xs" variant="default" onClick={() => setEditing(group)}>
                          Edit
                        </Button>
                        <Button size="xs" color="red" variant="light" onClick={() => remove(group)}>
                          Delete
                        </Button>
                      </Group>
                    </Table.Td>
                  )}
                </Table.Tr>
              ))}
            </Table.Tbody>
          </Table>
        )}
      </Card>

      <AddGroupModal opened={adding} onClose={() => setAdding(false)} onSaved={mutate} />
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
    </Stack>
  );
}

function AddGroupModal({
  opened,
  onClose,
  onSaved,
}: {
  opened: boolean;
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
    <Modal opened={opened} onClose={onClose} title="Add group">
      <Stack>
        <TextInput label="Name" value={name} onChange={(e) => setName(e.currentTarget.value)} required />
        <TextInput label="Comment" value={comment} onChange={(e) => setComment(e.currentTarget.value)} />
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

function EditGroupModal({
  group,
  onClose,
  onSaved,
}: {
  group: GroupEntry;
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
    <Modal opened onClose={onClose} title={group.name}>
      <Stack>
        <TextInput
          label="Name"
          value={name}
          onChange={(e) => setName(e.currentTarget.value)}
          disabled={group.name === "Default"}
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
            Save
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}
