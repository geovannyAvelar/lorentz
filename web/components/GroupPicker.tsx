"use client";

import { Checkbox, Stack, Text, ScrollArea, Fieldset } from "@mantine/core";
import { useGroups } from "@/hooks/useGroups";

export function GroupPicker({
  value,
  onChange,
}: {
  value: number[];
  onChange: (groups: number[]) => void;
}) {
  const { groups, isLoading } = useGroups();

  function toggle(id: number) {
    onChange(
      value.includes(id) ? value.filter((g) => g !== id) : [...value, id],
    );
  }

  if (isLoading) return <Text size="xs" c="dimmed">Loading groups…</Text>;
  if (groups.length === 0)
    return <Text size="xs" c="dimmed">No groups defined yet.</Text>;

  return (
    <Fieldset legend="Groups" p="xs">
      <ScrollArea.Autosize mah={128}>
        <Stack gap={6}>
          {groups.map((group) => (
            <Checkbox
              key={group.id}
              checked={value.includes(group.id)}
              onChange={() => toggle(group.id)}
              label={
                <>
                  {group.name}
                  {!group.enabled && (
                    <Text span c="dimmed" size="sm">
                      {" "}
                      (disabled)
                    </Text>
                  )}
                </>
              }
            />
          ))}
        </Stack>
      </ScrollArea.Autosize>
    </Fieldset>
  );
}
