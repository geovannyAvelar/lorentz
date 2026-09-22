"use client";

import { useState } from "react";
import useSWR from "swr";
import { fetcher } from "@/lib/client";
import { Card, CardHeader } from "@/components/ui/Card";
import { Input, Select, Labeled } from "@/components/ui/Field";
import { Button } from "@/components/ui/Button";
import { QueryStatusBadge } from "@/components/ui/Badge";
import { Spinner, EmptyState } from "@/components/ui/Feedback";
import type { QueriesResponse } from "@/lib/types";

const LENGTHS = [25, 50, 100, 250];

function timeString(unixSeconds: number) {
  return new Date(unixSeconds * 1000).toLocaleString();
}

export default function QueriesPage() {
  const [domain, setDomain] = useState("");
  const [clientIp, setClientIp] = useState("");
  const [status, setStatus] = useState("");
  const [type, setType] = useState("");
  const [length, setLength] = useState(100);
  const [autoRefresh, setAutoRefresh] = useState(false);

  const params = new URLSearchParams();
  if (domain) params.set("domain", domain);
  if (clientIp) params.set("client_ip", clientIp);
  if (status) params.set("status", status);
  if (type) params.set("type", type);
  params.set("length", String(length));

  const { data, isLoading, mutate } = useSWR<QueriesResponse>(
    `/queries?${params.toString()}`,
    fetcher,
    { refreshInterval: autoRefresh ? 5000 : 0 },
  );

  return (
    <div className="flex flex-col gap-4">
      <Card>
        <CardHeader
          title="Query log"
          description={data ? `${data.recordsFiltered.toLocaleString()} of ${data.recordsTotal.toLocaleString()} queries` : undefined}
          actions={
            <>
              <label className="flex items-center gap-1.5 text-xs text-muted">
                <input
                  type="checkbox"
                  checked={autoRefresh}
                  onChange={(e) => setAutoRefresh(e.target.checked)}
                  className="h-3.5 w-3.5"
                />
                Auto-refresh
              </label>
              <Button size="sm" onClick={() => mutate()}>
                Refresh
              </Button>
            </>
          }
        />
        <div className="grid grid-cols-2 gap-3 p-4 sm:grid-cols-3 lg:grid-cols-5">
          <Labeled label="Domain" hint="Wildcards allowed, e.g. *.example.com">
            <Input value={domain} onChange={(e) => setDomain(e.target.value)} placeholder="example.com" />
          </Labeled>
          <Labeled label="Client IP">
            <Input value={clientIp} onChange={(e) => setClientIp(e.target.value)} placeholder="192.168.1.*" />
          </Labeled>
          <Labeled label="Status" hint="e.g. GRAVITY, FORWARDED, CACHE">
            <Input value={status} onChange={(e) => setStatus(e.target.value.toUpperCase())} placeholder="" />
          </Labeled>
          <Labeled label="Type" hint="e.g. A, AAAA, PTR">
            <Input value={type} onChange={(e) => setType(e.target.value.toUpperCase())} placeholder="" />
          </Labeled>
          <Labeled label="Rows">
            <Select value={length} onChange={(e) => setLength(Number(e.target.value))}>
              {LENGTHS.map((l) => (
                <option key={l} value={l}>
                  {l}
                </option>
              ))}
            </Select>
          </Labeled>
        </div>
      </Card>

      <Card className="overflow-hidden">
        {isLoading || !data ? (
          <div className="flex justify-center py-16">
            <Spinner />
          </div>
        ) : data.queries.length === 0 ? (
          <EmptyState title="No queries match these filters" />
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full min-w-[720px] text-left text-sm">
              <thead className="border-b border-border text-xs uppercase text-muted">
                <tr>
                  <th className="px-4 py-2 font-medium">Time</th>
                  <th className="px-4 py-2 font-medium">Type</th>
                  <th className="px-4 py-2 font-medium">Domain</th>
                  <th className="px-4 py-2 font-medium">Client</th>
                  <th className="px-4 py-2 font-medium">Status</th>
                  <th className="px-4 py-2 font-medium">Reply</th>
                  <th className="px-4 py-2 font-medium">Upstream</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-border">
                {data.queries.map((q) => (
                  <tr key={q.id}>
                    <td className="whitespace-nowrap px-4 py-2 text-muted">{timeString(q.time)}</td>
                    <td className="px-4 py-2">{q.type}</td>
                    <td className="max-w-xs truncate px-4 py-2 font-medium" title={q.domain}>
                      {q.domain}
                      {q.cname && (
                        <span className="ml-1 text-xs text-muted" title={`CNAME of ${q.cname}`}>
                          ↳ {q.cname}
                        </span>
                      )}
                    </td>
                    <td className="px-4 py-2 text-muted">{q.client.name || q.client.ip}</td>
                    <td className="px-4 py-2">
                      <QueryStatusBadge status={q.status} />
                    </td>
                    <td className="px-4 py-2 text-muted">{q.reply.type}</td>
                    <td className="px-4 py-2 text-muted">{q.upstream ?? "—"}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>
    </div>
  );
}
