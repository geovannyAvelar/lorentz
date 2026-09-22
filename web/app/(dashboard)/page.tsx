"use client";

import useSWR from "swr";
import { useState } from "react";
import { fetcher, api } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader, StatCard } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Badge } from "@/components/ui/Badge";
import { Spinner, EmptyState, ErrorBanner } from "@/components/ui/Feedback";
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
    <div className="flex flex-col gap-6">
      <Card>
        <CardHeader
          title="Blocking"
          description="Enable or disable Lorentz's DNS blocking"
          actions={
            blocking && (
              <>
                <Badge tone={blocking.blocking === "enabled" ? "success" : "danger"}>
                  {blocking.blocking}
                </Badge>
                {isAdmin && (
                  <Button
                    size="sm"
                    variant={blocking.blocking === "enabled" ? "danger" : "primary"}
                    loading={toggling}
                    onClick={toggleBlocking}
                    disabled={blocking.blocking === "failure"}
                  >
                    {blocking.blocking === "enabled" ? "Disable" : "Enable"}
                  </Button>
                )}
              </>
            )
          }
        />
        {isAdmin && toggleError && (
          <div className="px-4 py-3">
            <ErrorBanner>{toggleError}</ErrorBanner>
          </div>
        )}
      </Card>

      {loadingSummary || !q ? (
        <div className="flex justify-center py-12">
          <Spinner />
        </div>
      ) : (
        <div className="grid grid-cols-2 gap-4 sm:grid-cols-2 lg:grid-cols-4">
          <StatCard label="Queries (24h)" value={q.total.toLocaleString()} />
          <StatCard
            label="Blocked"
            value={`${q.blocked.toLocaleString()} (${q.percent_blocked.toFixed(1)}%)`}
          />
          <StatCard label="Unique domains" value={q.unique_domains.toLocaleString()} />
          <StatCard label="Cached" value={q.cached.toLocaleString()} />
        </div>
      )}

      <div className="grid grid-cols-1 gap-4 lg:grid-cols-2">
        <Card>
          <CardHeader title="Top blocked domains" description="Last 24 hours" />
          <TopList
            rows={topDomains?.domains.map((d) => ({ label: d.domain, count: d.count }))}
            empty="No blocked domains in this window."
          />
        </Card>
        <Card>
          <CardHeader title="Top clients" description="Last 24 hours" />
          <TopList
            rows={topClients?.clients.map((c) => ({
              label: c.name || c.ip,
              count: c.count,
            }))}
            empty="No client activity in this window."
          />
        </Card>
      </div>
    </div>
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
      <div className="flex justify-center py-8">
        <Spinner />
      </div>
    );
  }
  if (rows.length === 0) return <EmptyState title={empty} />;

  const max = Math.max(...rows.map((r) => r.count), 1);
  return (
    <ul className="divide-y divide-border">
      {rows.map((row) => (
        <li key={row.label} className="flex items-center gap-3 px-4 py-2.5">
          <span className="w-40 truncate text-sm text-foreground" title={row.label}>
            {row.label}
          </span>
          <div className="h-2 flex-1 overflow-hidden rounded-full bg-border/60">
            <div
              className="h-full rounded-full bg-accent"
              style={{ width: `${(row.count / max) * 100}%` }}
            />
          </div>
          <span className="w-12 text-right text-sm text-muted">{row.count}</span>
        </li>
      ))}
    </ul>
  );
}
