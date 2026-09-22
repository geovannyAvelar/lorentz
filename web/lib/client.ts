// The client-side counterpart of lib/lorentz-server.ts: every component in
// this app calls Lorentz through /api/lorentz/*, never directly.
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
  const res = await fetch(`/api/lorentz${path}`, {
    ...init,
    headers: { "Content-Type": "application/json", ...(init?.headers ?? {}) },
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
  del: <T>(path: string) => request<T>(path, { method: "DELETE" }),
};

// A SWR fetcher: useSWR("/queries?length=10", fetcher)
export const fetcher = <T>(path: string) => api.get<T>(path);
