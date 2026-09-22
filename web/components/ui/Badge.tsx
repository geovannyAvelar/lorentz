import type { ReactNode } from "react";
import { cn } from "@/lib/cn";

type Tone = "neutral" | "success" | "danger" | "warning" | "accent";

const TONES: Record<Tone, string> = {
  neutral: "bg-border/60 text-foreground",
  success: "bg-success/15 text-success",
  danger: "bg-danger/15 text-danger",
  warning: "bg-warning/15 text-warning",
  accent: "bg-accent/15 text-accent",
};

export function Badge({
  children,
  tone = "neutral",
  className,
}: {
  children: ReactNode;
  tone?: Tone;
  className?: string;
}) {
  return (
    <span
      className={cn(
        "inline-flex items-center rounded-full px-2 py-0.5 text-xs font-medium",
        TONES[tone],
        className,
      )}
    >
      {children}
    </span>
  );
}

// Lorentz's query status strings aren't a fixed enum the API promises to keep
// closed (get_query_status_str() in datastructure.c), so this matches on
// substrings instead of an exhaustive switch and falls back to neutral for
// anything it doesn't recognize.
export function QueryStatusBadge({ status }: { status: string }) {
  const upper = status.toUpperCase();
  let tone: Tone = "neutral";
  if (/GRAVITY|DENY|BLACKLIST|BLOCK/.test(upper)) tone = "danger";
  else if (/ALLOW|WHITELIST/.test(upper)) tone = "success";
  else if (upper.includes("CACHE")) tone = "accent";
  else if (upper.includes("FORWARD")) tone = "accent";
  else if (/UNKNOWN|RETRIED|ERROR/.test(upper)) tone = "warning";
  return <Badge tone={tone}>{status}</Badge>;
}
