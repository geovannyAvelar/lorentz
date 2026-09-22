"use client";

import useSWR from "swr";
import { Card, Group, Text, Table, Button, Center, Loader, EmptyState, ScrollArea } from "@mantine/core";
import { api, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import type { NetworkResponse } from "@/lib/types";

function timeString(unixSeconds: number) {
  return new Date(unixSeconds * 1000).toLocaleString();
}

export default function NetworkPage() {
  const { isAdmin } = useSession();
  const { data, isLoading, mutate } = useSWR<NetworkResponse>("/network/devices", fetcher);

  async function remove(id: number, hwaddr: string) {
    if (!confirm(`Remove device ${hwaddr} and its IP addresses from the network table?`)) return;
    await api.del(`/network/devices/${id}`);
    mutate();
  }

  return (
    <Card withBorder padding={0}>
      <Group justify="space-between" p="md">
        <div>
          <Text fw={600}>Network</Text>
          <Text size="xs" c="dimmed">Devices Lorentz has seen on your network</Text>
        </div>
      </Group>
      {isLoading || !data ? (
        <Center py="xl">
          <Loader />
        </Center>
      ) : data.devices.length === 0 ? (
        <EmptyState title="No devices recorded yet" p="xl" />
      ) : (
        <ScrollArea>
          <Table striped highlightOnHover verticalSpacing="xs">
            <Table.Thead>
              <Table.Tr>
                <Table.Th>Device</Table.Th>
                <Table.Th>IP addresses</Table.Th>
                <Table.Th>Interface</Table.Th>
                <Table.Th>Queries</Table.Th>
                <Table.Th>Last seen</Table.Th>
                {isAdmin && <Table.Th>Actions</Table.Th>}
              </Table.Tr>
            </Table.Thead>
            <Table.Tbody>
              {data.devices.map((device) => (
                <Table.Tr key={device.id}>
                  <Table.Td>
                    <Text ff="monospace" size="xs">{device.hwaddr}</Text>
                    {device.macVendor && (
                      <Text size="xs" c="dimmed">{device.macVendor}</Text>
                    )}
                  </Table.Td>
                  <Table.Td c="dimmed">
                    {device.ips.map((ip) => (
                      <Text size="sm" key={ip.ip}>
                        {ip.ip}
                        {ip.name && <Text span size="xs"> ({ip.name})</Text>}
                      </Text>
                    ))}
                  </Table.Td>
                  <Table.Td c="dimmed">{device.interface}</Table.Td>
                  <Table.Td c="dimmed">{device.numQueries.toLocaleString()}</Table.Td>
                  <Table.Td c="dimmed">{timeString(device.lastQuery)}</Table.Td>
                  {isAdmin && (
                    <Table.Td>
                      <Button size="xs" color="red" variant="light" onClick={() => remove(device.id, device.hwaddr)}>
                        Delete
                      </Button>
                    </Table.Td>
                  )}
                </Table.Tr>
              ))}
            </Table.Tbody>
          </Table>
        </ScrollArea>
      )}
    </Card>
  );
}
