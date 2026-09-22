"use client";

import useSWR from "swr";
import { useState } from "react";
import {
  Card,
  Group,
  Text,
  Badge,
  Button,
  SimpleGrid,
  Stack,
  Progress,
  Alert,
  Center,
  Loader,
  EmptyState,
} from "@mantine/core";
import { IconAlertCircle } from "@tabler/icons-react";
import { fetcher, api } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import type {
  BlockingStatus,
  StatsSummary,
  TopDomainsResponse,
  TopClientsResponse,
} from "@/lib/types";

function windowParams(hours: number) {
  const now = Math.floor(Date.now() / 1000);
  return `from=${now - hours * 3600}&until=${now}`;
}

export default function DashboardPage() {
  const { isAdmin } = useSession();
  const { data: summary, isLoading: loadingSummary } = useSWR<StatsSummary>(
    "/stats/summary",
    fetcher,
    { refreshInterval: 15_000 },
  );
  const { data: blocking, mutate: mutateBlocking } = useSWR<BlockingStatus>(
    "/dns/blocking",
    fetcher,
    { refreshInterval: 15_000 },
  );
  const window24h = windowParams(24);
  const { data: topDomains } = useSWR<TopDomainsResponse>(
    `/stats/database/top_domains?${window24h}&count=5&blocked=true`,
    fetcher,
  );
  const { data: topClients } = useSWR<TopClientsResponse>(
    `/stats/database/top_clients?${window24h}&count=5`,
    fetcher,
  );

  const [toggling, setToggling] = useState(false);
  const [toggleError, setToggleError] = useState<string | null>(null);

  async function toggleBlocking() {
    if (!blocking) return;
    setToggling(true);
    setToggleError(null);
    try {
      const updated = await api.post<BlockingStatus>("/dns/blocking", {
        blocking: blocking.blocking !== "enabled",
      });
      mutateBlocking(updated, false);
    } catch {
      setToggleError("Could not change the blocking status");
    } finally {
      setToggling(false);
    }
  }

  const q = summary?.queries;

  return (
    <Stack gap="lg">
      <Card withBorder>
        <Group justify="space-between">
          <div>
            <Text fw={600}>Blocking</Text>
            <Text size="xs" c="dimmed">
              Enable or disable Lorentz&apos;s DNS blocking
            </Text>
          </div>
          {blocking && (
            <Group gap="xs">
              <Badge color={blocking.blocking === "enabled" ? "green" : "red"}>
                {blocking.blocking}
              </Badge>
              {isAdmin && (
                <Button
                  size="xs"
                  color={blocking.blocking === "enabled" ? "red" : "blue"}
                  loading={toggling}
                  onClick={toggleBlocking}
                  disabled={blocking.blocking === "failure"}
                >
                  {blocking.blocking === "enabled" ? "Disable" : "Enable"}
                </Button>
              )}
            </Group>
          )}
        </Group>
        {isAdmin && toggleError && (
          <Alert color="red" icon={<IconAlertCircle size={16} />} mt="sm">
            {toggleError}
          </Alert>
        )}
      </Card>

      {loadingSummary || !q ? (
        <Center py="xl">
          <Loader />
        </Center>
      ) : (
        <SimpleGrid cols={{ base: 2, lg: 4 }}>
          <StatCard label="Queries (24h)" value={q.total.toLocaleString()} />
          <StatCard
            label="Blocked"
            value={`${q.blocked.toLocaleString()} (${q.percent_blocked.toFixed(1)}%)`}
          />
          <StatCard label="Unique domains" value={q.unique_domains.toLocaleString()} />
          <StatCard label="Cached" value={q.cached.toLocaleString()} />
        </SimpleGrid>
      )}

      <SimpleGrid cols={{ base: 1, lg: 2 }}>
        <Card withBorder padding={0}>
          <Group justify="space-between" p="md" pb="xs">
            <div>
              <Text fw={600}>Top blocked domains</Text>
              <Text size="xs" c="dimmed">Last 24 hours</Text>
            </div>
          </Group>
          <TopList
            rows={topDomains?.domains.map((d) => ({ label: d.domain, count: d.count }))}
            empty="No blocked domains in this window."
          />
        </Card>
        <Card withBorder padding={0}>
          <Group justify="space-between" p="md" pb="xs">
            <div>
              <Text fw={600}>Top clients</Text>
              <Text size="xs" c="dimmed">Last 24 hours</Text>
            </div>
          </Group>
          <TopList
            rows={topClients?.clients.map((c) => ({
              label: c.name || c.ip,
              count: c.count,
            }))}
            empty="No client activity in this window."
          />
        </Card>
      </SimpleGrid>
    </Stack>
  );
}

function StatCard({ label, value }: { label: string; value: string }) {
  return (
    <Card withBorder padding="md">
      <Text size="xs" c="dimmed" tt="uppercase" fw={600}>
        {label}
      </Text>
      <Text size="xl" fw={700} mt={4}>
        {value}
      </Text>
    </Card>
  );
}

function TopList({
  rows,
  empty,
}: {
  rows: { label: string; count: number }[] | undefined;
  empty: string;
}) {
  if (!rows) {
    return (
      <Center py="lg">
        <Loader size="sm" />
      </Center>
    );
  }
  if (rows.length === 0) return <EmptyState title={empty} p="lg" />;

  const max = Math.max(...rows.map((r) => r.count), 1);
  return (
    <Stack gap={0}>
      {rows.map((row) => (
        <Group key={row.label} px="md" py="xs" wrap="nowrap" gap="sm">
          <Text size="sm" w={160} truncate="end" title={row.label}>
            {row.label}
          </Text>
          <Progress value={(row.count / max) * 100} flex={1} />
          <Text size="sm" c="dimmed" w={40} ta="right">
            {row.count}
          </Text>
        </Group>
      ))}
    </Stack>
  );
}
