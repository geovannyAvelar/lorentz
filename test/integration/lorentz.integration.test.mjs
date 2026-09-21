// Integration tests for lorentz. They run the real binary in a container
// and check, from the outside, that Lorentz behaves normally: it starts and
// migrates its databases, answers and blocks DNS queries, serves the API,
// keeps its data across a restart and shuts down without database errors.
//
// The point of the suite is to catch regressions in everything that sits on
// top of the database layer (long-term database, in-memory query database,
// gravity database, sessions, network table, messages, Teleporter), so most
// checks go through the public interfaces (DNS and the REST API) and only
// verify the databases directly where an interface does not show the result.
//
// Run with: npm --prefix test/integration install && npm --prefix test/integration test
// (build Lorentz first, see LORENTZ_BINARY in lib.mjs). Needs a running Docker daemon.
import { after, before, describe, it } from "node:test";
import assert from "node:assert/strict";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import {
  LORENTZ_DB, GRAVITY_DB, api, apiBinary, buildImage, dig, eventually, lorentzLog,
  restartLorentz, run, settle, sleep, sqlite, startLorentz, startPostgres, waitForApi,
} from "./lib.mjs";

const BLOCKED = ["0.0.0.0"];

// Names in the gravity database of the sample data (test/gravity.db.sql)
const GRAVITY_DOMAIN = "gravity.lorentz"; // in the gravity table
const DENIED_DOMAIN = "denied.lorentz"; // exact denylist
const ALLOWED_DOMAIN = "allowed.lorentz"; // exact allowlist
const LOCAL_DOMAIN = "local.example.com"; // local record set through dns.hosts

// SQLite reports "recovered N frames from WAL file" (code 283, a notice) after
// Lorentz restarted itself, e.g. following a Teleporter import, and Lorentz logs SQLite
// messages at ERROR level. It is expected there and says nothing about the
// database, everything else at ERROR level is a problem
const isWalRecovery = (line) => /recovered \d+ frames from WAL file/.test(line);
const errorLines = (log) => log.split("\n").filter((line) => /\bERROR\b/.test(line) && !isWalRecovery(line));

// The long-term database Lorentz is run with. SQLite is the default, PostgreSQL
// needs a Lorentz built with -DUSE_POSTGRESQL=ON. LORENTZ_BACKENDS=sqlite,postgres
// selects (default: both)
const wanted = (process.env.LORENTZ_BACKENDS ?? "sqlite,postgres").split(",");
let postgres;

before(async () => {
  await buildImage();
  if (wanted.includes("postgres")) postgres = await startPostgres();
});
after(async () => {
  await postgres?.stop();
});

const backends = [];
if (wanted.includes("sqlite"))
  backends.push({
    name: "sqlite",
    // Start Lorentz. longterm.query() runs SQL on its long-term database
    async start(environment = {}, options = {}) {
      const lorentz = await startLorentz(environment, options);
      lorentz.longterm = { query: (sql) => sqlite(lorentz, LORENTZ_DB, sql) };
      return lorentz;
    },
    // The long-term database is intact
    healthy: async (lorentz) => assert.equal(await lorentz.longterm.query("PRAGMA integrity_check;"), "ok"),
  });
if (wanted.includes("postgres"))
  backends.push({
    name: "postgres",
    async start(environment = {}, options = {}) {
      const schema = await postgres.schema();
      const lorentz = await startLorentz(
        { LORENTZCONF_files_database: schema.uri, ...environment },
        { ...options, network: postgres.network });
      lorentz.longterm = { query: schema.sql, schema: schema.name };
      return lorentz;
    },
    healthy: async (lorentz) => assert.equal(await lorentz.longterm.query("SELECT 1"), "1"),
  });

for (const backend of backends) describe(`a fresh lorentz (${backend.name})`, () => {
  let lorentz;

  before(async () => {
    lorentz = await backend.start();
  });
  after(async () => {
    await lorentz?.stop();
  });

  describe("startup", () => {
    it("creates the long-term database from scratch and reports no errors", async () => {
      const log = await lorentzLog(lorentz);
      if (backend.name === "sqlite") {
        assert.match(log, /Database version is 1\b/);
        assert.match(log, /Updating long-term database to version 22/);
      } else {
        assert.match(log, /Creating the long-term database \(version 22\)/);
      }
      assert.match(log, /Database successfully initialized/);
      assert.match(log, /Imported 0 queries from the (on-disk|long-term) database/);
      assert.deepEqual(errorLines(log), [], "lorentz.log contains ERROR lines");
    });

    it("leaves a healthy long-term database behind", async () => {
      await backend.healthy(lorentz);
      const db = lorentz.longterm;
      const version = Number(await db.query("SELECT value FROM lorentz WHERE id = 0;"));
      assert.ok(version >= 22, `database version ${version}`);
      const tables = (backend.name === "sqlite"
        ? await db.query("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;")
        : await db.query(`SELECT table_name FROM information_schema.tables WHERE table_schema = '${db.schema}' AND table_type = 'BASE TABLE' ORDER BY 1`)
      ).split("\n");
      for (const table of ["query_storage", "domain_by_id", "client_by_id", "forward_by_id",
        "addinfo_by_id", "message", "session", "network", "network_addresses", "aliasclient", "counters", "lorentz"])
        assert.ok(tables.includes(table), `table ${table} exists`);
      assert.equal(await db.query(backend.name === "sqlite"
        ? "SELECT count(*) FROM sqlite_master WHERE type = 'view' AND name = 'queries';"
        : `SELECT count(*) FROM information_schema.views WHERE table_schema = '${db.schema}' AND table_name = 'queries'`), "1");
    });

    it("reads the gravity database", async () => {
      const { status, body } = await api(lorentz, "/api/info/lorentz");
      assert.equal(status, 200);
      assert.equal(body.lorentz.database.gravity, 8);
      assert.equal(body.lorentz.database.antigravity, 2);
      assert.ok(body.lorentz.database.groups >= 1);
      assert.ok(body.lorentz.database.clients >= 1);
      assert.equal(body.lorentz.database.domains.denied.total, 2);
    });

    if (backend.name === "sqlite") it("reports the SQLite version", async () => {
      const { status, body } = await api(lorentz, "/api/info/version");
      assert.equal(status, 200);
      assert.match(body.version.lorentz.local.version, /\S/);
      const sqliteVersion = await run(lorentz, ["lorentz", "sqlite3", "-batch", ":memory:", "SELECT sqlite_version();"]);
      assert.match(sqliteVersion, /^3\.\d+\.\d+/);
    });
  });

  describe("DNS", () => {
    it("answers from local records", async () => {
      assert.deepEqual(await dig(lorentz, LOCAL_DOMAIN), ["1.2.3.4"]);
    });

    it("blocks a domain from gravity", async () => {
      assert.deepEqual(await dig(lorentz, GRAVITY_DOMAIN), BLOCKED);
    });

    it("blocks a domain on the exact denylist", async () => {
      assert.deepEqual(await dig(lorentz, DENIED_DOMAIN), BLOCKED);
    });

    it("does not block a domain on the exact allowlist", async () => {
      assert.notDeepEqual(await dig(lorentz, ALLOWED_DOMAIN), BLOCKED);
    });

    it("records the decisions in the query log", async () => {
      const result = await eventually("queries in the query log", async () => {
        const { body } = await api(lorentz, "/api/queries?length=100");
        return body.queries.length >= 4 ? body : null;
      });
      const status = (domain) => result.queries.find((q) => q.domain === domain)?.status;
      assert.equal(status(GRAVITY_DOMAIN), "GRAVITY");
      assert.equal(status(DENIED_DOMAIN), "DENYLIST");
      assert.ok(status(LOCAL_DOMAIN), "local record was logged");
      assert.ok(status(ALLOWED_DOMAIN), "allowed domain was logged");
    });

    it("filters the query log", async () => {
      const gravity = await api(lorentz, `/api/queries?domain=${GRAVITY_DOMAIN}`);
      assert.equal(gravity.body.queries.length, 1);
      assert.equal(gravity.body.queries[0].client.ip, "127.0.0.1");

      // A wildcard is a LIKE, which ignores the case of letters
      const wildcard = await api(lorentz, "/api/queries?domain=GRAVITY.lor*");
      assert.equal(wildcard.body.queries.length, 1);
      assert.equal(wildcard.body.queries[0].domain, GRAVITY_DOMAIN);

      const denied = await api(lorentz, "/api/queries?status=DENYLIST");
      assert.ok(denied.body.queries.every((q) => q.status === "DENYLIST"));
      assert.equal(denied.body.queries[0].domain, DENIED_DOMAIN);

      const limited = await api(lorentz, "/api/queries?length=1");
      assert.equal(limited.body.queries.length, 1);

      const none = await api(lorentz, "/api/queries?domain=nosuchdomain.invalid");
      assert.equal(none.body.queries.length, 0);
    });

    it("summarizes the traffic", async () => {
      const { status, body } = await api(lorentz, "/api/stats/summary");
      assert.equal(status, 200);
      assert.ok(body.queries.total >= 4);
      assert.ok(body.queries.blocked >= 2);
      assert.equal(body.queries.status.GRAVITY >= 1, true);
      assert.equal(body.queries.status.DENYLIST >= 1, true);
    });
  });

  describe("managing lists through the API", () => {
    it("adds, reads and removes a denylist entry", async () => {
      const domain = "blocked.example.org";
      assert.notDeepEqual(await dig(lorentz, domain), BLOCKED);

      const created = await api(lorentz, "/api/domains/deny/exact", {
        method: "POST", json: { domain, comment: "integration test", groups: [0], enabled: true },
      });
      assert.equal(created.status, 201);
      assert.equal(created.body.processed.errors.length, 0);

      const read = await api(lorentz, `/api/domains/deny/exact/${domain}`);
      assert.equal(read.body.domains[0].domain, domain);
      assert.equal(read.body.domains[0].comment, "integration test");
      assert.deepEqual(read.body.domains[0].groups, [0]);

      await eventually("the new entry to block", async () => (await dig(lorentz, domain))[0] === "0.0.0.0");

      const removed = await api(lorentz, `/api/domains/deny/exact/${domain}`, { method: "DELETE" });
      assert.equal(removed.status, 204);
      await eventually("the entry to be gone", async () => (await dig(lorentz, domain))[0] !== "0.0.0.0");
    });

    it("rejects an invalid regular expression", async () => {
      const { status, body } = await api(lorentz, "/api/domains/deny/regex", {
        method: "POST", json: { domain: "(", groups: [0], enabled: true },
      });
      assert.equal(status, 400);
      assert.equal(body.error.key, "regex_error");
    });

    it("resolves a client's groups when its configuration changes", async () => {
      // The sample data configures 127.0.0.1, the client of these tests. Move
      // it into a group that is on no list: gravity no longer applies to it
      const original = (await api(lorentz, "/api/clients/127.0.0.1")).body.clients[0];
      const group = await api(lorentz, "/api/groups", {
        method: "POST", json: { name: "not-blocked", comment: "integration test", enabled: true },
      });
      assert.equal(group.status, 201);
      const groupId = (await api(lorentz, "/api/groups/not-blocked")).body.groups[0].id;

      const moved = await api(lorentz, "/api/clients/127.0.0.1", {
        method: "PUT", json: { comment: original.comment, groups: [groupId] },
      });
      assert.equal(moved.status, 200);
      await eventually("gravity to stop applying to the client",
        async () => (await dig(lorentz, GRAVITY_DOMAIN))[0] !== "0.0.0.0", { timeoutMs: 30_000 });

      const restored = await api(lorentz, "/api/clients/127.0.0.1", {
        method: "PUT", json: { comment: original.comment, groups: original.groups },
      });
      assert.equal(restored.status, 200);
      await eventually("gravity to apply again",
        async () => (await dig(lorentz, GRAVITY_DOMAIN))[0] === "0.0.0.0", { timeoutMs: 30_000 });
      assert.equal((await api(lorentz, "/api/groups/not-blocked", { method: "DELETE" })).status, 204);
    });

    it("picks up a rebuilt gravity database", async () => {
      const domain = "rebuilt.example";
      assert.notDeepEqual(await dig(lorentz, domain), BLOCKED);
      await settle(lorentz); // no reload may be running while gravity.db is written
      await sqlite(lorentz, GRAVITY_DB,
        `INSERT INTO gravity(domain, adlist_id) VALUES ('${domain}', 1);
         UPDATE info SET value = CAST(strftime('%s','now') AS INTEGER) + 5 WHERE property = 'updated';`);
      await eventually("Lorentz to notice the new gravity",
        async () => (await dig(lorentz, domain))[0] === "0.0.0.0", { timeoutMs: 30_000 });
      await eventually("the reload to be logged",
        async () => /Gravity database has been updated, reloading now/.test(await lorentzLog(lorentz)));
    });
  });

  describe("network table and messages", () => {
    it("records the client as a network device", async () => {
      const devices = await eventually("the client in the network table", async () => {
        const { body } = await api(lorentz, "/api/network/devices");
        return body.devices?.length ? body.devices : null;
      }, { timeoutMs: 30_000 });
      const device = devices.find((d) => d.ips.some((ip) => ip.ip === "127.0.0.1"));
      assert.ok(device, "127.0.0.1 is a known device");
      assert.ok(device.numQueries > 0);
      assert.equal(device.interface, "lo");
    });

    it("lists and deletes messages", async () => {
      const list = await api(lorentz, "/api/info/messages");
      assert.equal(list.status, 200);
      const count = await api(lorentz, "/api/info/messages/count");
      assert.equal(count.body.count, list.body.messages.length);
      assert.ok(list.body.messages.length >= 1, "Lorentz warns about the missing upstream servers");
      const [message] = list.body.messages;
      assert.ok(message.plain && message.html);

      const removed = await api(lorentz, `/api/info/messages/${message.id}`, { method: "DELETE" });
      assert.equal(removed.status, 204);
      const after = await api(lorentz, "/api/info/messages/count");
      assert.equal(after.body.count, count.body.count - 1);
    });
  });

  describe("Teleporter", () => {
    it("exports an archive and imports it again", async () => {
      const archive = await apiBinary(lorentz, "/api/teleporter");
      assert.equal(archive.status, 200);
      assert.equal(archive.data.subarray(0, 2).toString(), "PK", "the archive is a ZIP file");
      assert.ok(archive.data.length > 1000);

      const form = new FormData();
      form.append("file", new Blob([archive.data]), "teleporter.zip");
      const imported = await api(lorentz, "/api/teleporter", { method: "POST", body: form });
      assert.equal(imported.status, 200);
      assert.ok(imported.body.files.some((file) => file.startsWith("etc/lorentz/gravity.db->")));

      // Lorentz restarts its DNS engine after an import
      await waitForApi(lorentz);
      await eventually("gravity to apply after the import",
        async () => (await dig(lorentz, GRAVITY_DOMAIN))[0] === "0.0.0.0", { timeoutMs: 45_000 });
      assert.equal(await sqlite(lorentz, GRAVITY_DB, "PRAGMA integrity_check;"), "ok");
    });
  });

  describe("restart", () => {
    it("keeps the query history in the long-term database", async () => {
      // Traffic in a known amount, then the totals before the restart
      for (const domain of [GRAVITY_DOMAIN, DENIED_DOMAIN, LOCAL_DOMAIN, ALLOWED_DOMAIN])
        await dig(lorentz, domain);
      const before = await eventually("all queries logged", async () => {
        const { body } = await api(lorentz, "/api/queries?length=1");
        return body.recordsTotal >= 8 ? body : null;
      });

      await restartLorentz(lorentz);

      // The shutdown exports everything, the start imports it again
      const log = await lorentzLog(lorentz);
      assert.match(log, /Imported \d+ queries from the (on-disk|long-term) database/);
      const stored = Number(await lorentz.longterm.query("SELECT count(*) FROM query_storage;"));
      assert.ok(stored >= before.recordsTotal, `${stored} stored, ${before.recordsTotal} expected`);

      const after = await api(lorentz, "/api/queries?length=1");
      assert.ok(after.body.recordsTotal >= before.recordsTotal, "history survived the restart");
      const gravity = await api(lorentz, `/api/queries?domain=${GRAVITY_DOMAIN}`);
      assert.ok(gravity.body.queries.length >= 1);
      assert.equal(gravity.body.queries[0].status, "GRAVITY");

      // And Lorentz keeps working
      assert.deepEqual(await dig(lorentz, GRAVITY_DOMAIN), BLOCKED);
      await backend.healthy(lorentz);
    });

    it("serves statistics from the long-term database", async () => {
      const now = Math.floor(Date.now() / 1000);
      const window = `from=${now - 3600}&until=${now + 3600}`;

      const summary = await api(lorentz, `/api/stats/database/summary?${window}`);
      assert.equal(summary.status, 200);
      assert.ok(summary.body.sum_queries >= 8);
      assert.ok(summary.body.sum_blocked >= 2);
      assert.ok(summary.body.total_clients >= 1);

      const domains = await api(lorentz, `/api/stats/database/top_domains?${window}`);
      assert.ok(domains.body.domains.some((d) => d.domain === LOCAL_DOMAIN));
      const blocked = await api(lorentz, `/api/stats/database/top_domains?${window}&blocked=true`);
      assert.ok(blocked.body.domains.some((d) => d.domain === GRAVITY_DOMAIN));
      const clients = await api(lorentz, `/api/stats/database/top_clients?${window}`);
      assert.equal(clients.body.clients[0].ip, "127.0.0.1");

      const history = await api(lorentz, `/api/history/database?${window}`);
      assert.ok(history.body.history.length >= 1);
      assert.ok(history.body.history.reduce((sum, bucket) => sum + bucket.total, 0) >= 8);
      const clientHistory = await api(lorentz, `/api/history/database/clients?${window}`);
      assert.ok(Object.keys(clientHistory.body.clients).includes("127.0.0.1"));
      const types = await api(lorentz, `/api/stats/database/query_types?${window}`);
      assert.equal(types.status, 200);
      assert.ok(Object.keys(types.body.types).includes("A"));
      const upstreams = await api(lorentz, `/api/stats/database/upstreams?${window}`);
      assert.equal(upstreams.status, 200);
    });

    it("shuts down without database errors", async () => {
      await restartLorentz(lorentz);
      const log = await lorentzLog(lorentz);
      assert.deepEqual(errorLines(log), [], "lorentz.log contains ERROR lines");
      assert.doesNotMatch(log, /Statement not finalized/);
      assert.doesNotMatch(log, /SQL error/i);
      assert.doesNotMatch(log, /is damaged/);
      assert.doesNotMatch(log, /is read-only/);
    });
  });
});

for (const backend of backends) describe(`API sessions (${backend.name})`, () => {
  let lorentz;
  const password = "integration-test-password";

  before(async () => {
    lorentz = await backend.start({ LORENTZCONF_webserver_api_password: password });
  });
  after(async () => {
    await lorentz?.stop();
  });

  it("rejects a request without a session", async () => {
    const { status } = await api(lorentz, "/api/config/dns/upstreams");
    assert.equal(status, 401);
  });

  it("logs in and keeps the session across a restart", async () => {
    const wrong = await api(lorentz, "/api/auth", { method: "POST", json: { password: "wrong" } });
    assert.equal(wrong.status, 401);

    const login = await api(lorentz, "/api/auth", { method: "POST", json: { password } });
    assert.equal(login.status, 200);
    assert.equal(login.body.session.valid, true);
    const { sid } = login.body.session;
    assert.ok(sid.length > 10);
    assert.equal((await api(lorentz, "/api/config/dns/upstreams", { headers: { sid } })).status, 200);

    // The sessions are stored in the long-term database on shutdown
    await restartLorentz(lorentz);
    assert.match(await lorentzLog(lorentz), /Restored 1 API session/);
    const check = await api(lorentz, "/api/auth", { headers: { sid } });
    assert.equal(check.status, 200);
    assert.equal(check.body.session.valid, true);
    assert.equal((await api(lorentz, "/api/config/dns/upstreams", { headers: { sid } })).status, 200);

    // Logging out ends it for good
    assert.equal((await api(lorentz, "/api/auth", { method: "DELETE", headers: { sid } })).status, 204);
    assert.equal((await api(lorentz, "/api/config/dns/upstreams", { headers: { sid } })).status, 401);
  });
});

describe("upgrading a database", () => {
  let lorentz;
  const fixture = join(dirname(fileURLToPath(import.meta.url)), "..", "lorentz.db.sql");

  // The long-term database the bats suite starts from is a version 9 database
  // with real rows. Rebuild it before Lorentz starts, then let Lorentz migrate it
  before(async () => {
    lorentz = await startLorentz({}, {
      files: [{ source: fixture, target: "/tmp/lorentz.db.sql" }],
      command: ["sh", "-c",
        "lorentz sqlite3 /etc/lorentz/lorentz.db < /tmp/lorentz.db.sql && exec lorentz no-daemon"],
    });
  });
  after(async () => {
    await lorentz?.stop();
  });

  it("migrates every version up to the current one", async () => {
    const log = await lorentzLog(lorentz);
    assert.match(log, /Database version is 9\b/);
    assert.match(log, /Updating long-term database to version 10/);
    assert.match(log, /Updating long-term database to version 22/);
    assert.match(log, /Database successfully initialized/);
    assert.deepEqual(errorLines(log), []);
    assert.ok(Number(await sqlite(lorentz, LORENTZ_DB, "SELECT value FROM lorentz WHERE id = 0;")) >= 22);
    assert.equal(await sqlite(lorentz, LORENTZ_DB, "PRAGMA integrity_check;"), "ok");
  });

  it("keeps the network table and the alias-clients", async () => {
    const { status, body } = await api(lorentz, "/api/network/devices");
    assert.equal(status, 200);
    const device = body.devices.find((d) => d.hwaddr === "aa:bb:cc:dd:ee:ff");
    assert.ok(device, "the device of the old database is still there");
    assert.equal(device.interface, "lo123");
    assert.deepEqual(device.ips.map((ip) => ip.ip).sort(), ["127.0.0.4", "127.0.0.5"]);
    const other = body.devices.find((d) => d.hwaddr === "00:11:22:33:44:55");
    assert.deepEqual(other.ips.map((ip) => ip.ip), ["127.0.0.6"]);
    assert.equal(await sqlite(lorentz, LORENTZ_DB, "SELECT name FROM aliasclient WHERE id = 0;"), "some-aliasclient");
    // The old query table became a view over the new storage tables
    assert.equal(await sqlite(lorentz, LORENTZ_DB, "SELECT count(*) FROM queries;"),
      await sqlite(lorentz, LORENTZ_DB, "SELECT count(*) FROM query_storage;"));
  });

  it("works after the migration", async () => {
    assert.deepEqual(await dig(lorentz, GRAVITY_DOMAIN), BLOCKED);
    assert.deepEqual(await dig(lorentz, LOCAL_DOMAIN), ["1.2.3.4"]);
    const { status } = await api(lorentz, "/api/stats/summary");
    assert.equal(status, 200);
    const stats = await api(lorentz, "/api/stats/database/summary?from=1&until=2000000000");
    assert.equal(stats.status, 200);
  });
});
