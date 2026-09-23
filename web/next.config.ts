import type { NextConfig } from "next";
import path from "node:path";

// LORENTZ_EMBED=1 npm run build (see package.json's "build:embed") produces a
// static export that Lorentz's C build embeds directly into the `lorentz`
// binary (src/api/webui/) and serves at its own admin path - see
// README.md, "Web UI". Everything else (`npm run dev`, `npm run build` /
// `npm start`) runs this app as an ordinary Next.js server that proxies
// /api/* to a real Lorentz instance via rewrites(), so the browser only ever
// talks to this app's own origin either way.
const embedBuild = process.env.LORENTZ_EMBED === "1";

const nextConfig: NextConfig = {
  // The Lorentz repository has its own package-lock.json at its root (for the
  // C project's dev tooling). Pin Turbopack's root to this directory so it
  // doesn't guess wrong between the two lockfiles.
  turbopack: {
    root: path.resolve(__dirname),
  },
  ...(embedBuild
    ? {
        // Lorentz's default admin path (config.webserver.paths.webhome). A
        // Lorentz configured with a different webhome needs the standalone
        // deployment below instead - see web/README.md.
        output: "export" as const,
        trailingSlash: true,
        basePath: "/admin",
        images: { unoptimized: true },
      }
    : {
        // A self-contained build (node_modules pruned to what's actually
        // used) for the Dockerfile - see README.md.
        output: "standalone" as const,
        async rewrites() {
          const lorentzApiUrl = (process.env.LORENTZ_API_URL ?? "http://127.0.0.1").replace(/\/+$/, "");
          return [{ source: "/api/:path*", destination: `${lorentzApiUrl}/api/:path*` }];
        },
      }),
};

export default nextConfig;
