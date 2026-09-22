"use client";

import useSWR from "swr";
import { fetcher } from "@/lib/client";
import type { GroupsResponse } from "@/lib/types";

// Shared by every page that assigns items to groups (domains, lists, clients).
export function useGroups() {
  const { data, error, isLoading, mutate } = useSWR<GroupsResponse>(
    "/groups",
    fetcher,
  );
  return { groups: data?.groups ?? [], error, isLoading, mutate };
}
