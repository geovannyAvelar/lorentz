# Lorentz web UI

A [Next.js](https://nextjs.org) admin console for a Lorentz instance: dashboard,
query log, domains, groups, lists, clients, network devices and user accounts.
Built on the REST API in `../src/api` (see `../src/api/docs` for the full
reference, served by Lorentz itself at `/api/docs`). The design system is
[Mantine](https://mantine.dev) - components come straight from `@mantine/core`,
with `lib/theme.ts` holding the handful of theme tweaks (primary color, font,
default radius) and no other styling layer alongside it.

## Architecture

Every page calls Lorentz's own REST API directly at `/api/*` - there is no
backend-for-frontend layer in this app. That works out to the same origin in
every deployment mode:

- **Embedded** (`LORENTZ_EMBED=1 npm run build:embed`, or `-DEMBED_WEBUI=ON`
  on the Lorentz build): a static export, folded straight into the `lorentz`
  binary and served by Lorentz itself (`src/api/webui/`, in the main
  repository). Browser and API are literally the same server; see the root
  `README.md`, "Web UI".
- **Dev (`npm run dev`) and standalone (`npm run build` / `npm start`)**:
  an ordinary Next.js server. `next.config.ts`'s `rewrites()` proxies
  `/api/*` to `LORENTZ_API_URL` (a real Lorentz instance) transparently,
  including the `Set-Cookie` Lorentz sends back - so the browser still only
  ever talks to this app's own origin, and Lorentz still needs no CORS
  configuration.

Authentication is Lorentz's own cookie-based session (`POST /api/auth`, see
`src/api/auth.c`): an `httpOnly` `sid` cookie the browser sends automatically,
plus a CSRF token this app keeps in memory (`lib/client.ts`'s
`setCsrfToken()`, populated by `useSession()` from the session object) and
echoes back as `X-CSRF-TOKEN` on every call except `/api/auth` itself.
Roles are not cached beyond that: `useSession()` re-reads `/api/auth` every 60
seconds and on window focus, so a role change or a disabled account is picked
up without a fresh login.

Lorentz's default `Content-Security-Policy` forbids inline scripts
(`script-src 'self'`), which Next's static export needs for its hydration
bootstrap and Mantine's color-scheme initializer. Rather than relaxing that
to `'unsafe-inline'`, `npm run build:embed` runs
`scripts/compute-csp-hashes.mjs` right after `next build`, which hashes each
page's inline `<script>` blocks; `src/api/webui/generate.sh` (in the main
repository) embeds those hashes alongside each file, and `webui_handler()`
(`src/api/webui/webui.c`) splices them into that page's own `script-src`
instead of sending the configured header unchanged. Every build's hashes
differ (the hydration payload does), so this is meaningless outside the
embedded build - the standalone/dev server doesn't set this CSP at all.

## Development

Requires Node.js 20 or newer.

```bash
npm install
LORENTZ_API_URL=http://127.0.0.1 npm run dev
```

Open <http://localhost:3000>. On a Lorentz instance with no account and no
`webserver.api.password` set, the login page offers to create the first
(admin) account instead of a password field - the API is "open" until then
(see `database/user-table.c`, `users_login_required()`).

## Environment variables

| Variable | Where | Purpose |
|---|---|---|
| `LORENTZ_API_URL` | server only | Base URL of the Lorentz instance, e.g. `http://lorentz:80`. Defaults to `http://127.0.0.1`. |

There is nothing to configure client-side: the browser only ever sees this
app's own origin.

## Docker

This standalone image is for a Lorentz configured with a non-default
`webserver.paths.webhome` (the embedded build always serves at `/admin/`,
see the root `README.md`) or for running the UI as its own service:

```bash
docker build -t lorentz-web web/
docker run -p 3000:3000 -e LORENTZ_API_URL=http://lorentz:80 lorentz-web
```

A minimal Compose fragment, alongside the `lorentz` service itself:

```yaml
services:
  lorentz:
    image: lorentz # build from the repository root
    ports: ["53:53/udp", "80:80"]
  web:
    build: ./web
    environment:
      LORENTZ_API_URL: http://lorentz:80
    ports: ["3000:3000"]
    depends_on: [lorentz]
```

## What isn't here

Scoped out of this first pass, all doable against the existing API without
further backend changes:

- Teleporter (export/import), DHCP leases, and the full settings editor
  (`/api/config` has ~170 keys).
- Triggering a gravity update from the UI - `/api/action/gravity` streams
  plain-text output over one long chunked HTTP response and appends a
  trailing JSON status to the same body, which doesn't fit a JSON-based proxy
  cleanly; run `pihole -g` (or the Lorentz equivalent) directly instead.
- Two-factor authentication at login (`webserver.api.totp_secret` covers only
  the configured password, not accounts, so this matters less than it would
  otherwise).

## Manual verification

There is no automated test suite for this app yet (see "What isn't here"
above for the reasoning on scope; adding Playwright coverage similar to
`../test/integration` would be the natural next step). It was exercised by
hand against `lorentz-integration-test:local` (see `../test/integration`)
through the whole flow: bootstrap, login with both an account and the
configured password, CRUD on domains/groups/lists/clients, network device
listing and deletion, the query log with filters, the blocking toggle, and
account self-service (password change, comment) - each checked against both
an admin and a viewer account, including two 403s where the backend rejects
what a viewer's UI does not offer as a button.

`npm run build`, `npm run build:embed` and `npm run lint` are clean (Next.js
16, React 19, strict TypeScript). The embedded build was additionally
exercised end to end via `docker build -f ../Dockerfile ..`
(`-DEMBED_WEBUI=ON`) and a container run of the result: `/admin/` and its
`_next/static` assets serve correctly, `/admin` and `/` redirect as expected,
an unmatched path 404s, and the full native auth cycle
(`POST /api/users` to bootstrap, `POST /api/auth` to log in, a
CSRF-protected `GET /api/users`, `DELETE /api/auth` to log out) works exactly
as `lib/client.ts` and `hooks/useSession.ts` assume.
