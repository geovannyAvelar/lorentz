"use client";

import type { ReactNode } from "react";
import {
  Badge,
  Button,
  Group,
  Menu,
  NumberInput,
  PasswordInput,
  Select,
  Switch,
  TagsInput,
  Text,
  TextInput,
  Tooltip,
} from "@mantine/core";
import { IconChevronDown, IconInfoCircle } from "@tabler/icons-react";
import { firstParagraph, isPassword, isSecretString, options } from "@/lib/config-schema";
import type { ConfigEntry } from "@/lib/config-schema";
import type { DnsServerPreset } from "@/lib/types";

interface Props {
  entry: ConfigEntry;
  value: unknown;
  onChange: (value: unknown) => void;
  // Why this key cannot be edited here, if it cannot
  lockedReason?: string;
  // Suggested resolvers, offered as a menu on dns.upstreams
  presets?: DnsServerPreset[];
}

// Adds a provider's addresses to the ones already listed, skipping repeats
function UpstreamPresets({
  presets,
  value,
  onChange,
  disabled,
}: {
  presets: DnsServerPreset[];
  value: string[];
  onChange: (value: string[]) => void;
  disabled: boolean;
}) {
  const add = (addresses: string[]) =>
    onChange([...value, ...addresses.filter((a) => !value.includes(a))]);
  const withV6 = presets.filter((p) => p.v6.length > 0);

  return (
    <Menu shadow="md" width={280} position="bottom-start" disabled={disabled}>
      <Menu.Target>
        <Button size="xs" variant="default" rightSection={<IconChevronDown size={14} />} disabled={disabled}>
          Add a provider
        </Button>
      </Menu.Target>
      <Menu.Dropdown mah={320} style={{ overflowY: "auto" }}>
        <Menu.Label>IPv4</Menu.Label>
        {presets.map((p) => (
          <Menu.Item key={`v4-${p.name}`} onClick={() => add(p.v4)}>
            {p.name}
          </Menu.Item>
        ))}
        <Menu.Divider />
        <Menu.Label>IPv6</Menu.Label>
        {withV6.map((p) => (
          <Menu.Item key={`v6-${p.name}`} onClick={() => add(p.v6)}>
            {p.name}
          </Menu.Item>
        ))}
      </Menu.Dropdown>
    </Menu>
  );
}

function Label({ entry, lockedReason }: { entry: ConfigEntry; lockedReason?: string }) {
  return (
    <Group gap={6} wrap="nowrap">
      <span>{entry.leaf}</span>
      <Tooltip
        multiline
        w={420}
        withArrow
        label={<Text size="xs" style={{ whiteSpace: "pre-wrap" }}>{entry.meta.description.trim()}</Text>}
      >
        <IconInfoCircle size={14} style={{ opacity: 0.6 }} />
      </Tooltip>
      {entry.meta.modified && (
        <Badge size="xs" variant="light">
          modified
        </Badge>
      )}
      {lockedReason && (
        <Tooltip label={lockedReason} withArrow multiline w={300}>
          <Badge size="xs" variant="outline" color="gray">
            locked
          </Badge>
        </Tooltip>
      )}
    </Group>
  );
}

function Hint({ entry }: { entry: ConfigEntry }): ReactNode {
  const { meta } = entry;
  if (isPassword(meta)) return "Leave empty to keep the current password";
  const text = firstParagraph(meta.description);
  return meta.modified ? `${text} (default: ${JSON.stringify(meta.default)})` : text;
}

export function ConfigField({ entry, value, onChange, lockedReason, presets }: Props) {
  const { meta } = entry;
  const disabled = lockedReason !== undefined;
  const common = {
    label: <Label entry={entry} lockedReason={lockedReason} />,
    description: <Hint entry={entry} />,
    disabled,
  };

  if (meta.type === "boolean") {
    return (
      <Switch
        {...common}
        checked={value === true}
        onChange={(e) => onChange(e.currentTarget.checked)}
      />
    );
  }

  if (meta.type.startsWith("enum")) {
    const choices = options(meta);
    const numeric = meta.type.includes("integer");
    const selected = choices.find((c) => String(c.item) === String(value));
    return (
      <>
        <Select
          {...common}
          allowDeselect={false}
          data={choices.map((c) => ({ value: String(c.item), label: String(c.item) }))}
          value={value === null || value === undefined ? null : String(value)}
          onChange={(v) => v !== null && onChange(numeric ? Number(v) : v)}
        />
        {selected && (
          <Text size="xs" c="dimmed" mt={4}>
            {selected.description}
          </Text>
        )}
      </>
    );
  }

  if (meta.type === "string array") {
    const list = Array.isArray(value) ? (value as string[]) : [];
    return (
      <>
        <TagsInput
          {...common}
          // The default also splits on ",", which some entries legitimately
          // contain (dns.cnameRecords, dns.revServers)
          splitChars={[]}
          acceptValueOnBlur
          value={list}
          onChange={onChange}
        />
        {entry.key === "dns.upstreams" && presets && presets.length > 0 && (
          <Group mt={6}>
            <UpstreamPresets presets={presets} value={list} onChange={onChange} disabled={disabled} />
          </Group>
        )}
      </>
    );
  }

  if (meta.type === "double" || meta.type.includes("integer")) {
    return (
      <NumberInput
        {...common}
        allowDecimal={meta.type === "double"}
        min={meta.type.startsWith("unsigned") ? 0 : undefined}
        max={meta.type.includes("16 bit") ? 65535 : undefined}
        value={typeof value === "number" ? value : ""}
        onChange={(v) => onChange(v === "" ? null : v)}
      />
    );
  }

  if (isPassword(meta)) {
    return (
      <PasswordInput
        {...common}
        autoComplete="new-password"
        value={typeof value === "string" ? value : ""}
        onChange={(e) => onChange(e.currentTarget.value)}
      />
    );
  }

  // A secret that the API does send back (a hash, or a URI holding a
  // password): masked, with the eye toggle to see or edit it
  if (isSecretString(entry)) {
    return (
      <PasswordInput
        {...common}
        autoComplete="off"
        value={typeof value === "string" ? value : ""}
        onChange={(e) => onChange(e.currentTarget.value)}
      />
    );
  }

  // string, IPv4 address, IPv6 address
  return (
    <TextInput
      {...common}
      value={typeof value === "string" ? value : String(value ?? "")}
      onChange={(e) => onChange(e.currentTarget.value)}
    />
  );
}
