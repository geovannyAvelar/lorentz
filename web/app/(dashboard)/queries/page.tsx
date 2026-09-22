"use client";

import { useState } from "react";
import useSWR from "swr";
import {
  Card,
  Group,
  Stack,
  Text,
  TextInput,
  NativeSelect,
  Checkbox,
  Button,
  Table,
  Center,
  Loader,
  EmptyState,
  ScrollArea,
} from "@mantine/core";
import { IconRefresh } from "@tabler/icons-react";
import { fetcher } from "@/lib/client";
import { QueryStatusBadge } from "@/components/QueryStatusBadge";
import type { QueriesResponse } from "@/lib/types";

const LENGTHS = ["25", "50", "100", "250"];

function timeString(unixSeconds: number) {
  return new Date(unixSeconds * 1000).toLocaleString();
}

export default function QueriesPage() {
  const [domain, setDomain] = useState("");
  const [clientIp, setClientIp] = useState("");
  const [status, setStatus] = useState("");
  const [type, setType] = useState("");
  const [length, setLength] = useState("100");
  const [autoRefresh, setAutoRefresh] = useState(false);

  const params = new URLSearchParams();
  if (domain) params.set("domain", domain);
  if (clientIp) params.set("client_ip", clientIp);
  if (status) params.set("status", status);
  if (type) params.set("type", type);
  params.set("length", length);

  const { data, isLoading, mutate } = useSWR<QueriesResponse>(
    `/queries?${params.toString()}`,
    fetcher,
    { refreshInterval: autoRefresh ? 5000 : 0 },
  );

  return (
    <Stack>
      <Card withBorder>
        <Group justify="space-between" mb="sm">
          <div>
            <Text fw={600}>Query log</Text>
            {data && (
              <Text size="xs" c="dimmed">
                {data.recordsFiltered.toLocaleString()} of {data.recordsTotal.toLocaleString()} queries
              </Text>
            )}
          </div>
          <Group gap="sm">
            <Checkbox
              size="xs"
              label="Auto-refresh"
              checked={autoRefresh}
              onChange={(e) => setAutoRefresh(e.currentTarget.checked)}
            />
            <Button size="xs" leftSection={<IconRefresh size={14} />} onClick={() => mutate()}>
              Refresh
            </Button>
          </Group>
        </Group>
        <Group grow align="flex-start">
          <TextInput
            label="Domain"
            description="Wildcards allowed, e.g. *.example.com"
            value={domain}
            onChange={(e) => setDomain(e.currentTarget.value)}
            placeholder="example.com"
          />
          <TextInput
            label="Client IP"
            value={clientIp}
            onChange={(e) => setClientIp(e.currentTarget.value)}
            placeholder="192.168.1.*"
          />
          <TextInput
            label="Status"
            description="e.g. GRAVITY, FORWARDED, CACHE"
            value={status}
            onChange={(e) => setStatus(e.currentTarget.value.toUpperCase())}
          />
          <TextInput
            label="Type"
            description="e.g. A, AAAA, PTR"
            value={type}
            onChange={(e) => setType(e.currentTarget.value.toUpperCase())}
          />
          <NativeSelect
            label="Rows"
            data={LENGTHS}
            value={length}
            onChange={(e) => setLength(e.currentTarget.value)}
          />
        </Group>
      </Card>

      <Card withBorder padding={0}>
        {isLoading || !data ? (
          <Center py="xl">
            <Loader />
          </Center>
        ) : data.queries.length === 0 ? (
          <EmptyState title="No queries match these filters" p="xl" />
        ) : (
          <ScrollArea>
            <Table striped highlightOnHover verticalSpacing="xs" miw={720}>
              <Table.Thead>
                <Table.Tr>
                  <Table.Th>Time</Table.Th>
                  <Table.Th>Type</Table.Th>
                  <Table.Th>Domain</Table.Th>
                  <Table.Th>Client</Table.Th>
                  <Table.Th>Status</Table.Th>
                  <Table.Th>Reply</Table.Th>
                  <Table.Th>Upstream</Table.Th>
                </Table.Tr>
              </Table.Thead>
              <Table.Tbody>
                {data.queries.map((q) => (
                  <Table.Tr key={q.id}>
                    <Table.Td c="dimmed" style={{ whiteSpace: "nowrap" }}>
                      {timeString(q.time)}
                    </Table.Td>
                    <Table.Td>{q.type}</Table.Td>
                    <Table.Td maw={280} style={{ overflow: "hidden", textOverflow: "ellipsis" }}>
                      <Text fw={500} truncate="end" title={q.domain}>
                        {q.domain}
                        {q.cname && (
                          <Text span c="dimmed" size="xs" ml={4} title={`CNAME of ${q.cname}`}>
                            ↳ {q.cname}
                          </Text>
                        )}
                      </Text>
                    </Table.Td>
                    <Table.Td c="dimmed">{q.client.name || q.client.ip}</Table.Td>
                    <Table.Td>
                      <QueryStatusBadge status={q.status} />
                    </Table.Td>
                    <Table.Td c="dimmed">{q.reply.type}</Table.Td>
                    <Table.Td c="dimmed">{q.upstream ?? "—"}</Table.Td>
                  </Table.Tr>
                ))}
              </Table.Tbody>
            </Table>
          </ScrollArea>
        )}
      </Card>
    </Stack>
  );
}
