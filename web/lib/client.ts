// Talks to Lorentz directly: the browser and Lorentz are the same origin
// (this app is either Lorentz's own embedded web UI, or proxied to Lorentz
// one-for-one by next.config.ts's rewrites() in dev/standalone mode), so no
// BFF is needed. Authentication is Lorentz's own cookie: an httpOnly "sid"
// cookie the browser sends automatically, plus a CSRF token that has to be
// echoed back as X-CSRF-TOKEN on every /api/* call except /api/auth itself
// (see check_client_auth() in src/api/auth.c). useSession() captures that
// token from the session object and hands it to setCsrfToken() below.
let csrfToken: string | null = null;

export function setCsrfToken(token: string | null | undefined) {
  csrfToken = token ?? null;
}

export class ApiError extends Error {
  status: number;
  key?: string;
  hint?: string | null;

  constructor(status: number, message: string, key?: string, hint?: string | null) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.key = key;
    this.hint = hint;
  }
}

interface ErrorBody {
  error?: { key?: string; message?: string; hint?: string | null };
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const headers: Record<string, string> = {
    "Content-Type": "application/json",
    ...(init?.headers as Record<string, string> | undefined),
  };
  // /api/auth is exempt from the CSRF check (it's how a client without a
  // token yet logs in or checks its status), everything else needs it once
  // a session exists.
  if (csrfToken && path !== "/auth") headers["X-CSRF-TOKEN"] = csrfToken;

  const res = await fetch(`/api${path}`, {
    ...init,
    headers,
    cache: "no-store",
  });

  const text = await res.text();
  let data: unknown = null;
  if (text.length > 0) {
    try {
      data = JSON.parse(text);
    } catch {
      // A handful of error paths answer with plain text; res.ok is false
      // there too, so the message below still surfaces something useful.
    }
  }

  if (!res.ok) {
    if (res.status === 401) setCsrfToken(null);
    const err = (data as ErrorBody | null)?.error;
    throw new ApiError(
      res.status,
      err?.message ?? (typeof data === "string" ? data : res.statusText),
      err?.key,
      err?.hint,
    );
  }
  return data as T;
}

export const api = {
  get: <T>(path: string) => request<T>(path),
  post: <T>(path: string, body?: unknown) =>
    request<T>(path, {
      method: "POST",
      body: body !== undefined ? JSON.stringify(body) : undefined,
    }),
  put: <T>(path: string, body?: unknown) =>
    request<T>(path, {
      method: "PUT",
      body: body !== undefined ? JSON.stringify(body) : undefined,
    }),
  patch: <T>(path: string, body?: unknown) =>
    request<T>(path, {
      method: "PATCH",
      body: body !== undefined ? JSON.stringify(body) : undefined,
    }),
  del: <T>(path: string) => request<T>(path, { method: "DELETE" }),
};

// A SWR fetcher: useSWR("/queries?length=10", fetcher)
export const fetcher = <T>(path: string) => api.get<T>(path);
