// Configuration shared between the route handlers that talk to Lorentz.
// LORENTZ_API_URL is a server-only setting (Docker Compose, a systemd unit
// environment file, ...): the browser never sees it and never talks to
// Lorentz directly, only to this app's own routes. That is also why Lorentz
// itself needs no CORS support - every request to it is server-to-server.
export const LORENTZ_API_URL = (
  process.env.LORENTZ_API_URL ?? "http://127.0.0.1"
).replace(/\/+$/, "");

// Name of the cookie this app sets after a successful login. It carries the
// Lorentz session id (sid), set httpOnly so client-side JavaScript - and so
// an XSS bug - cannot read it. Lorentz accepts a sid via a plain header
// (see check_client_auth() in api/auth.c), which is what the proxy sends, so
// this cookie is never forwarded to Lorentz as a Cookie header and the CSRF
// token Lorentz requires for cookie-based auth never comes up.
export const SESSION_COOKIE = "lorentz_sid";
