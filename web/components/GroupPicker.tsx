"use client";

import { useGroups } from "@/hooks/useGroups";
import { Checkbox } from "@/components/ui/Field";

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

  if (isLoading) return <p className="text-xs text-muted">Loading groups…</p>;
  if (groups.length === 0)
    return <p className="text-xs text-muted">No groups defined yet.</p>;

  return (
    <div className="flex max-h-32 flex-col gap-1 overflow-y-auto rounded-md border border-border p-2">
      {groups.map((group) => (
        <Checkbox
          key={group.id}
          checked={value.includes(group.id)}
          onChange={() => toggle(group.id)}
          label={
            <span>
              {group.name}
              {!group.enabled && (
                <span className="ml-1 text-muted">(disabled)</span>
              )}
            </span>
          }
        />
      ))}
    </div>
  );
}
