import type { NextConfig } from "next";
import path from "node:path";

const nextConfig: NextConfig = {
  // The Lorentz repository has its own package-lock.json at its root (for the
  // C project's dev tooling). Pin Turbopack's root to this directory so it
  // doesn't guess wrong between the two lockfiles.
  turbopack: {
    root: path.resolve(__dirname),
  },
  // A self-contained build (node_modules pruned to what's actually used) for
  // the Dockerfile - see README.md.
  output: "standalone",
};

export default nextConfig;
