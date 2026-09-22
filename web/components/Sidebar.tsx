"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { cn } from "@/lib/cn";

interface NavItem {
  href: string;
  label: string;
  adminOnly?: boolean;
}

const NAV: NavItem[] = [
  { href: "/", label: "Dashboard" },
  { href: "/queries", label: "Query log" },
  { href: "/domains", label: "Domains" },
  { href: "/groups", label: "Groups" },
  { href: "/lists", label: "Lists" },
  { href: "/clients", label: "Clients" },
  { href: "/network", label: "Network" },
  { href: "/users", label: "Users", adminOnly: true },
];

export function Sidebar({ isAdmin }: { isAdmin: boolean }) {
  const pathname = usePathname();

  return (
    <nav className="flex w-56 shrink-0 flex-col gap-1 border-r border-border bg-surface p-3">
      <div className="mb-2 px-2 py-1">
        <span className="text-base font-semibold text-foreground">Lorentz</span>
      </div>
      {NAV.filter((item) => !item.adminOnly || isAdmin).map((item) => {
        const active =
          item.href === "/" ? pathname === "/" : pathname.startsWith(item.href);
        return (
          <Link
            key={item.href}
            href={item.href}
            className={cn(
              "rounded-md px-2.5 py-1.5 text-sm font-medium transition-colors",
              active
                ? "bg-accent/15 text-accent"
                : "text-foreground hover:bg-border/50",
            )}
          >
            {item.label}
          </Link>
        );
      })}
    </nav>
  );
}
