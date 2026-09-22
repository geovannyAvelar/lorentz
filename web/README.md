# Lorentz web UI

A [Next.js](https://nextjs.org) admin console for a Lorentz instance: dashboard,
query log, domains, groups, lists, clients, network devices and user accounts.
Built on the REST API in `../src/api` (see `../src/api/docs` for the full
reference, served by Lorentz itself at `/api/docs`).

## Architecture

The browser never talks to Lorentz directly. Every page calls this app's own
routes under `/api/lorentz/*`, `/api/session` and `/api/bootstrap`, which
proxy to Lorentz server-side (see `lib/lorentz-server.ts`). That keeps the
Lorentz API URL, and the session id it authenticates with, off the client:

- **`LORENTZ_API_URL`** is a server-only environment variable. It is never
  sent to the browser, so Lorentz needs no CORS configuration - the only
  requests it ever sees are server-to-server, from this app.
- **The session** is a Lorentz `sid`, kept in an `httpOnly` cookie set by
  `app/api/session/route.ts`. Client-side JavaScript cannot read it. It is
  sent to Lorentz as a plain `sid` header (Lorentz accepts that in addition to
  a cookie, see `check_client_auth()` in `src/api/auth.c`), not as a `Cookie`
  header - so the CSRF token Lorentz requires for cookie-based auth never
  applies here.
- **`proxy.ts`** (Next.js 16 renamed `middleware.ts`) redirects a
  cookie-less visitor to `/login` before any protected page renders. That is
  a convenience only; the real enforcement is Lorentz's own 401/403 on every
  API call, which every page already reacts to (see `useSession()` and the
  `isAdmin` checks sprinkled through `app/(dashboard)/*`).
- **Roles** are not cached client-side beyond the current page load:
  `useSession()` re-reads `/api/session` (which re-reads `/api/auth`) every
  60 seconds and on window focus, so a role change or a disabled account is
  picked up without a fresh login.

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

`npm run build` and `npm run lint` are clean (Next.js 16, React 19, strict
TypeScript).
