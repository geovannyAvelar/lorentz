import { Badge, type MantineColor } from "@mantine/core";

// Lorentz's query status strings aren't a fixed enum the API promises to keep
// closed (get_query_status_str() in datastructure.c), so this matches on
// substrings instead of an exhaustive switch and falls back to gray for
// anything it doesn't recognize.
export function QueryStatusBadge({ status }: { status: string }) {
  const upper = status.toUpperCase();
  let color: MantineColor = "gray";
  if (/GRAVITY|DENY|BLACKLIST|BLOCK/.test(upper)) color = "red";
  else if (/ALLOW|WHITELIST/.test(upper)) color = "green";
  else if (upper.includes("CACHE")) color = "blue";
  else if (upper.includes("FORWARD")) color = "indigo";
  else if (/UNKNOWN|RETRIED|ERROR/.test(upper)) color = "yellow";
  return (
    <Badge color={color} variant="light">
      {status}
    </Badge>
  );
}
