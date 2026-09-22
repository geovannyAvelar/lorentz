"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { NavLink, Stack } from "@mantine/core";
import {
  IconLayoutDashboard,
  IconList,
  IconBan,
  IconUsersGroup,
  IconListDetails,
  IconDevices,
  IconNetwork,
  IconUserCog,
} from "@tabler/icons-react";

interface NavItem {
  href: string;
  label: string;
  icon: typeof IconLayoutDashboard;
  adminOnly?: boolean;
}

const NAV: NavItem[] = [
  { href: "/", label: "Dashboard", icon: IconLayoutDashboard },
  { href: "/queries", label: "Query log", icon: IconList },
  { href: "/domains", label: "Domains", icon: IconBan },
  { href: "/groups", label: "Groups", icon: IconUsersGroup },
  { href: "/lists", label: "Lists", icon: IconListDetails },
  { href: "/clients", label: "Clients", icon: IconDevices },
  { href: "/network", label: "Network", icon: IconNetwork },
  { href: "/users", label: "Users", icon: IconUserCog, adminOnly: true },
];

export function Sidebar({ isAdmin }: { isAdmin: boolean }) {
  const pathname = usePathname();

  return (
    <Stack gap={2}>
      {NAV.filter((item) => !item.adminOnly || isAdmin).map((item) => {
        const active =
          item.href === "/" ? pathname === "/" : pathname.startsWith(item.href);
        const Icon = item.icon;
        return (
          <NavLink
            key={item.href}
            component={Link}
            href={item.href}
            label={item.label}
            leftSection={<Icon size={18} stroke={1.5} />}
            active={active}
            variant="light"
          />
        );
      })}
    </Stack>
  );
}
