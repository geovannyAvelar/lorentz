// Shapes of the Lorentz API responses this app reads. Kept to the fields the
// UI actually uses; see src/api/docs/content/specs/*.yaml in the Lorentz
// repository for the full, authoritative schema of each endpoint.

export type Role = "admin" | "viewer";

export interface SessionUser {
  id: number;
  username: string;
  role: Role;
}

export interface Session {
  valid: boolean;
  totp: boolean;
  validity: number;
  message: string | null;
  user: SessionUser | null;
}

export interface User {
  id: number;
  username: string;
  role: Role;
  enabled: boolean;
  comment: string | null;
  created_at: number;
  updated_at: number;
  last_login: number | null;
}

export interface DomainEntry {
  id: number;
  domain: string;
  unicode?: string;
  type: "allow" | "deny";
  kind: "exact" | "regex";
  comment: string | null;
  groups: number[];
  enabled: boolean;
  date_added: number;
  date_modified: number;
}

export interface Group {
  id: number;
  name: string;
  comment: string | null;
  enabled: boolean;
  date_added: number;
  date_modified: number;
}

export interface AdList {
  id: number;
  address: string;
  type: "allow" | "block";
  comment: string | null;
  groups: number[];
  enabled: boolean;
  date_added: number;
  date_modified: number;
  number?: number;
  invalid_domains?: number;
  date_updated?: number;
  status?: number;
}

export interface Client {
  client: string;
  comment: string | null;
  groups: number[];
  id: number;
  date_added: number;
  date_modified: number;
  name?: string | null;
}

export interface NetworkAddress {
  ip: string;
  name: string | null;
  lastSeen: number;
  nameUpdated: number;
}

export interface NetworkDevice {
  id: number;
  hwaddr: string;
  interface: string;
  firstSeen: number;
  lastQuery: number;
  numQueries: number;
  macVendor: string | null;
  ips: NetworkAddress[];
}

export interface QueryEntry {
  id: number;
  time: number;
  type: string;
  status: string;
  domain: string;
  upstream: string | null;
  reply: { type: string; time: number };
  client: { ip: string; name: string | null };
  list_id: number | null;
  ede: { code: number; text: string | null };
  cname: string | null;
}

export interface QueriesResponse {
  queries: QueryEntry[];
  cursor: number | null;
  recordsTotal: number;
  recordsFiltered: number;
  draw: number;
}

export interface StatsSummary {
  queries: {
    total: number;
    blocked: number;
    percent_blocked: number;
    unique_domains: number;
    forwarded: number;
    cached: number;
    frequency: number;
    types: Record<string, number>;
    status: Record<string, number>;
  };
  clients: { active: number; total: number };
  gravity: { domains_being_blocked: number; last_update: number };
}

export interface DatabaseSummary {
  sum_queries: number;
  sum_blocked: number;
  percent_blocked: number;
  total_clients: number;
}

export interface TopDomainsResponse {
  domains: { domain: string; count: number }[];
  total_queries: number;
  blocked_queries: number;
}

export interface TopClientsResponse {
  clients: { ip: string; name: string | null; count: number }[];
  total_queries: number;
  blocked_queries: number;
}

export interface HistoryBucket {
  timestamp: number;
  total: number;
  cached: number;
  blocked: number;
  forwarded: number;
}

export interface BlockingStatus {
  blocking: "enabled" | "disabled" | "failure" | "unknown";
  timer: number | null;
}

export interface Message {
  id: number;
  timestamp: number;
  type: string;
  plain: string;
  html: string;
}

// GET responses wrap their array in an object named after the resource, plus
// a "took" timing field this app ignores.
export type DomainsResponse = { domains: DomainEntry[] };
export type GroupsResponse = { groups: Group[] };
export type ListsResponse = { lists: AdList[] };
export type ClientsResponse = { clients: Client[] };
export type NetworkResponse = { devices: NetworkDevice[] };
export type UsersResponse = { users: User[] };
export type MessagesResponse = { messages: Message[] };
