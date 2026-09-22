"use client";

import { SWRConfig } from "swr";
import type { ReactNode } from "react";
import { ApiError } from "@/lib/client";

export function SWRProvider({ children }: { children: ReactNode }) {
  return (
    <SWRConfig
      value={{
        onErrorRetry: (error, key, config, revalidate, opts) => {
          // Retrying an auth or permission error just repeats the same
          // answer: the pages themselves react to it (redirect to /login,
          // show "insufficient permissions").
          if (error instanceof ApiError && [401, 403, 404].includes(error.status))
            return;
          if (opts.retryCount >= 3) return;
          setTimeout(() => revalidate(opts), 2000 * (opts.retryCount + 1));
        },
      }}
    >
      {children}
    </SWRConfig>
  );
}
