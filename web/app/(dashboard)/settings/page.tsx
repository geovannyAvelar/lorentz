"use client";

import { useMemo, useState } from "react";
import useSWR from "swr";
import {
  Affix,
  Alert,
  Badge,
  Button,
  Card,
  Center,
  Group,
  Loader,
  Paper,
  Stack,
  Tabs,
  Text,
  TextInput,
} from "@mantine/core";
import { IconAlertCircle, IconCheck, IconDeviceFloppy, IconSearch } from "@tabler/icons-react";
import { api, ApiError, fetcher } from "@/lib/client";
import { useSession } from "@/hooks/useSession";
import { ConfigField } from "@/components/ConfigField";
import { buildPatch, flattenConfig, isPassword, isSame, originalValue } from "@/lib/config-schema";
import type { ConfigEntry } from "@/lib/config-schema";
import type { ConfigPropertiesResponse, ConfigResponse } from "@/lib/types";

const READ_ONLY_REASON =
  "Can only be set in lorentz.toml, through an environment variable or with lorentz --config, not via the API";
const ENV_REASON = "Set through an environment variable, which takes precedence over this page";

export default function SettingsPage() {
  const { isAdmin, isLoading: sessionLoading } = useSession();
  const swrOptions = { revalidateOnFocus: false };
  const { data, error, isLoading, mutate } = useSWR<ConfigResponse>(
    isAdmin ? "/config?detailed=true" : null,
    fetcher,
    swrOptions,
  );
  const { data: properties } = useSWR<ConfigPropertiesResponse>(
    isAdmin ? "/config/_properties" : null,
    fetcher,
    swrOptions,
  );

  // Edited values by key; a key is only here while it differs from the saved one
  const [drafts, setDrafts] = useState<Record<string, unknown>>({});
  const [section, setSection] = useState<string | null>(null);
  const [query, setQuery] = useState("");
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const entries = useMemo(() => (data ? flattenConfig(data.config) : []), [data]);
  const readOnly = useMemo(
    () => new Set(properties?.config.read_only.map((p) => p.key) ?? []),
    [properties],
  );
  const topics = useMemo(() => {
    const known = data?.topics ?? [];
    const names = new Set(known.map((t) => t.name));
    const extra = [...new Set(entries.map((e) => e.section))].filter((s) => !names.has(s));
    return [...known, ...extra.map((name) => ({ name, title: name, description: "" }))];
  }, [data, entries]);

  const byKey = useMemo(() => new Map(entries.map((e) => [e.key, e])), [entries]);
  const activeSection = section ?? topics[0]?.name ?? "";
  const searching = query.trim() !== "";

  const visible = useMemo(() => {
    const q = query.trim().toLowerCase();
    return entries.filter((e) =>
      q
        ? e.key.toLowerCase().includes(q) || e.meta.description.toLowerCase().includes(q)
        : e.section === activeSection,
    );
  }, [entries, query, activeSection]);

  function lockedReason(entry: ConfigEntry): string | undefined {
    if (readOnly.has(entry.key)) return READ_ONLY_REASON;
    if (entry.meta.flags.env_var) return ENV_REASON;
    return undefined;
  }

  function edit(entry: ConfigEntry, value: unknown) {
    setNotice(null);
    setDrafts((prev) => {
      const next = { ...prev };
      if (isSame(entry.meta, value)) delete next[entry.key];
      else next[entry.key] = value;
      return next;
    });
  }

  const dirtyKeys = Object.keys(drafts);
  const dirtyBySection = new Map<string, number>();
  for (const key of dirtyKeys) {
    const s = byKey.get(key)?.section ?? "";
    dirtyBySection.set(s, (dirtyBySection.get(s) ?? 0) + 1);
  }

  async function save() {
    setSaving(true);
    setSaveError(null);
    setNotice(null);
    const changed = dirtyKeys.map((k) => byKey.get(k)).filter((e): e is ConfigEntry => e !== undefined);
    try {
      await api.patch("/config", { config: buildPatch(drafts) });
      setDrafts({});
      await mutate();
      const parts = ["Settings saved."];
      if (changed.some((e) => e.meta.flags.restart_dnsmasq)) parts.push("The DNS resolver was restarted to apply them.");
      if (changed.some((e) => e.meta.flags.session_reset)) parts.push("Sessions were reset, sign in again.");
      setNotice(parts.join(" "));
    } catch (err) {
      setSaveError(
        err instanceof ApiError ? [err.message, err.hint].filter(Boolean).join(": ") : "Could not save the settings",
      );
    } finally {
      setSaving(false);
    }
  }

  if (sessionLoading) {
    return (
      <Center py="xl">
        <Loader />
      </Center>
    );
  }

  if (!isAdmin) {
    return (
      <Alert color="yellow" icon={<IconAlertCircle size={16} />}>
        Only administrators can read and change the settings.
      </Alert>
    );
  }

  if (error) {
    return (
      <Alert color="red" icon={<IconAlertCircle size={16} />}>
        {error instanceof ApiError ? error.message : "Could not load the settings"}
      </Alert>
    );
  }

  if (isLoading || !data) {
    return (
      <Center py="xl">
        <Loader />
      </Center>
    );
  }

  // Entries by group, in the order the API lists them
  const groups = new Map<string, ConfigEntry[]>();
  for (const entry of visible) {
    const title = searching ? entry.key.split(".").slice(0, -1).join(" › ") : entry.group.split(".").join(" › ");
    groups.set(title, [...(groups.get(title) ?? []), entry]);
  }

  const active = topics.find((t) => t.name === activeSection);

  return (
    <Stack pb={dirtyKeys.length > 0 ? 80 : 0}>
      <Group justify="space-between" align="flex-end">
        <div>
          <Text fw={600}>Settings</Text>
          <Text size="xs" c="dimmed">
            {searching ? `${visible.length} matching setting(s)` : active?.description}
          </Text>
        </div>
        <TextInput
          placeholder="Search settings"
          leftSection={<IconSearch size={14} />}
          value={query}
          onChange={(e) => setQuery(e.currentTarget.value)}
          w={280}
        />
      </Group>

      {notice && (
        <Alert color="green" icon={<IconCheck size={16} />} withCloseButton onClose={() => setNotice(null)}>
          {notice}
        </Alert>
      )}
      {saveError && (
        <Alert color="red" icon={<IconAlertCircle size={16} />} withCloseButton onClose={() => setSaveError(null)}>
          {saveError}
        </Alert>
      )}

      <Tabs value={activeSection} onChange={setSection}>
        {!searching && (
          <Tabs.List mb="md">
            {topics.map((topic) => (
              <Tabs.Tab
                key={topic.name}
                value={topic.name}
                rightSection={
                  dirtyBySection.get(topic.name) ? (
                    <Badge size="xs" circle>
                      {dirtyBySection.get(topic.name)}
                    </Badge>
                  ) : undefined
                }
              >
                {topic.title}
              </Tabs.Tab>
            ))}
          </Tabs.List>
        )}

        <Stack>
          {[...groups.entries()].map(([title, items]) => (
            <Card key={title || "general"} withBorder>
              <Text fw={500} mb="sm">
                {title || "General"}
              </Text>
              <Stack gap="md">
                {items.map((entry) => (
                  <ConfigField
                    key={entry.key}
                    entry={entry}
                    value={entry.key in drafts ? drafts[entry.key] : originalValue(entry.meta)}
                    onChange={(v) => edit(entry, v)}
                    lockedReason={lockedReason(entry)}
                    presets={data.dns_servers}
                  />
                ))}
              </Stack>
            </Card>
          ))}
          {visible.length === 0 && (
            <Text c="dimmed" size="sm">
              No settings match.
            </Text>
          )}
        </Stack>
      </Tabs>

      {dirtyKeys.length > 0 && (
        <Affix position={{ bottom: 20, right: 20 }}>
          <Paper withBorder shadow="md" p="sm" radius="md">
            <Group gap="sm">
              <Text size="sm">
                {dirtyKeys.length} unsaved change{dirtyKeys.length === 1 ? "" : "s"}
                {dirtyKeys.some((k) => byKey.get(k) && isPassword(byKey.get(k)!.meta)) ? " (incl. password)" : ""}
              </Text>
              <Button size="xs" variant="default" onClick={() => setDrafts({})} disabled={saving}>
                Discard
              </Button>
              <Button size="xs" leftSection={<IconDeviceFloppy size={14} />} onClick={save} loading={saving}>
                Save
              </Button>
            </Group>
          </Paper>
        </Affix>
      )}
    </Stack>
  );
}
