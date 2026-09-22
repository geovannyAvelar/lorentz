"use client";

import useSWR from "swr";
import { api, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { Card, CardHeader } from "@/components/ui/Card";
import { Button } from "@/components/ui/Button";
import { Spinner, EmptyState } from "@/components/ui/Feedback";
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
    <Card>
      <CardHeader title="Network" description="Devices Lorentz has seen on your network" />
      {isLoading || !data ? (
        <div className="flex justify-center py-16">
          <Spinner />
        </div>
      ) : data.devices.length === 0 ? (
        <EmptyState title="No devices recorded yet" />
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-left text-sm">
            <thead className="border-b border-border text-xs uppercase text-muted">
              <tr>
                <th className="px-4 py-2 font-medium">Device</th>
                <th className="px-4 py-2 font-medium">IP addresses</th>
                <th className="px-4 py-2 font-medium">Interface</th>
                <th className="px-4 py-2 font-medium">Queries</th>
                <th className="px-4 py-2 font-medium">Last seen</th>
                {isAdmin && <th className="px-4 py-2 font-medium">Actions</th>}
              </tr>
            </thead>
            <tbody className="divide-y divide-border">
              {data.devices.map((device) => (
                <tr key={device.id}>
                  <td className="px-4 py-2">
                    <div className="font-mono text-xs">{device.hwaddr}</div>
                    {device.macVendor && (
                      <div className="text-xs text-muted">{device.macVendor}</div>
                    )}
                  </td>
                  <td className="px-4 py-2 text-muted">
                    {device.ips.map((ip) => (
                      <div key={ip.ip}>
                        {ip.ip}
                        {ip.name && <span className="ml-1 text-xs">({ip.name})</span>}
                      </div>
                    ))}
                  </td>
                  <td className="px-4 py-2 text-muted">{device.interface}</td>
                  <td className="px-4 py-2 text-muted">{device.numQueries.toLocaleString()}</td>
                  <td className="px-4 py-2 text-muted">{timeString(device.lastQuery)}</td>
                  {isAdmin && (
                    <td className="px-4 py-2">
                      <Button size="sm" variant="danger" onClick={() => remove(device.id, device.hwaddr)}>
                        Delete
                      </Button>
                    </td>
                  )}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </Card>
  );
}
