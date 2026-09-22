"use client";

import { useState } from "react";
import useSWR from "swr";
import {
  Card,
  Group,
  Stack,
  Text,
  Table,
  Button,
  Modal,
  TextInput,
  Textarea,
  Alert,
  Center,
  Loader,
  EmptyState,
} from "@mantine/core";
import { IconPlus, IconAlertCircle } from "@tabler/icons-react";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
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
    <Stack>
      <Card withBorder padding={0}>
        <Group justify="space-between" p="md">
          <div>
            <Text fw={600}>Clients</Text>
            <Text size="xs" c="dimmed">
              Clients configured here get non-default group assignments
            </Text>
          </div>
          {isAdmin && (
            <Button size="xs" leftSection={<IconPlus size={14} />} onClick={() => setAdding(true)}>
              Add client
            </Button>
          )}
        </Group>
        {isLoading || !data ? (
          <Center py="xl">
            <Loader />
          </Center>
        ) : data.clients.length === 0 ? (
          <EmptyState
            title="No clients configured yet"
            description="Every client uses the default group until it is added here"
            p="xl"
          />
        ) : (
          <Table striped highlightOnHover verticalSpacing="xs">
            <Table.Thead>
              <Table.Tr>
                <Table.Th>Client</Table.Th>
                <Table.Th>Comment</Table.Th>
                <Table.Th>Groups</Table.Th>
                {isAdmin && <Table.Th>Actions</Table.Th>}
              </Table.Tr>
            </Table.Thead>
            <Table.Tbody>
              {data.clients.map((client) => (
                <Table.Tr key={client.id}>
                  <Table.Td ff="monospace" fz="xs">{client.client}</Table.Td>
                  <Table.Td c="dimmed">{client.comment || "—"}</Table.Td>
                  <Table.Td c="dimmed">{client.groups.length}</Table.Td>
                  {isAdmin && (
                    <Table.Td>
                      <Group gap="xs" wrap="nowrap">
                        <Button size="xs" variant="default" onClick={() => setEditing(client)}>
                          Edit
                        </Button>
                        <Button size="xs" color="red" variant="light" onClick={() => remove(client)}>
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

      <AddClientModal opened={adding} onClose={() => setAdding(false)} onSaved={mutate} />
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
    </Stack>
  );
}

function AddClientModal({
  opened,
  onClose,
  onSaved,
}: {
  opened: boolean;
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
    <Modal opened={opened} onClose={onClose} title="Add client">
      <Stack>
        <Textarea
          label="Client"
          description="IP, MAC, hostname or interface (name:eth0). One per line for several at once"
          value={client}
          onChange={(e) => setClient(e.currentTarget.value)}
          rows={3}
          required
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
    <Modal opened onClose={onClose} title={client.client}>
      <Stack>
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
            Save
          </Button>
        </Group>
      </Stack>
    </Modal>
  );
}
