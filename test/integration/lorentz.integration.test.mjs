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
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { GenericContainer, Network, Wait } from "testcontainers";

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

// The databases Lorentz is run with. SQLite is the default, PostgreSQL needs a
// Lorentz built with -DUSE_POSTGRESQL=ON. "postgres" keeps the long-term database
// on the server (the gravity database stays a file), "postgres+gravity" the
// gravity database (lists, groups, clients, domains) as well.
// LORENTZ_BACKENDS=sqlite,postgres,postgres+gravity selects (default: all)
const repo = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const wanted = (process.env.LORENTZ_BACKENDS ?? "sqlite,postgres,postgres+gravity").split(",");
const needsPostgres = wanted.some((name) => name.startsWith("postgres"));
let postgres;

before(async () => {
  await buildImage();
  if (needsPostgres) postgres = await startPostgres();
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

if (wanted.includes("postgres+gravity"))
  backends.push({
    name: "postgres+gravity",
    gravityOnServer: true,
    async start(environment = {}, options = {}) {
      const schema = await postgres.schema();
      const lorentz = await startLorentz(
        { LORENTZCONF_files_database: schema.uri, LORENTZCONF_files_gravity: schema.uri, ...environment },
        { ...options, network: postgres.network });
      lorentz.longterm = { query: schema.sql, schema: schema.name };
      lorentz.gravity = { query: schema.sql };
      // Lorentz has created the tables; fill them with the sample data of the
      // SQLite gravity.db and let it notice
      await eventually("the gravity tables", async () => (await schema.sql("SELECT count(*) FROM info")) !== "");
      const reloads = async () =>
        ((await lorentzLog(lorentz)).match(/Gravity database has been updated, reloading now/g) ?? []).length;
      const seen = await reloads();
      await schema.sql(gravitySeed());
      // Lorentz reads the counts and the lists it keeps in memory when it sees
      // the new "updated" time, a moment later (this does not go through the API,
      // which a test may have put behind a password)
      // Lorentz's first look at "updated" only records it, so if that came after the
      // seed there is nothing to notice: keep moving it on until a reload shows
      await eventually("Lorentz to notice the sample gravity", async () => {
        if ((await reloads()) > seen) return true;
        await schema.sql("UPDATE info SET value = (CAST(value AS BIGINT) + 1)::text WHERE property = 'updated'");
        return false;
      }, { timeoutMs: 45_000, intervalMs: 1000 });
      await settle(lorentz);
      return lorentz;
    },
    healthy: async (lorentz) => assert.equal(await lorentz.longterm.query("SELECT 1"), "1"),
  });

// The rows of the sample gravity database (test/gravity.db.sql), as statements a
// PostgreSQL gravity database takes: what follows the schema of that file, minus
// the properties the tables start with, then the sequences moved past the ids
// the rows brought along and a new "updated" time for Lorentz to notice
function gravitySeed() {
  const sql = readFileSync(join(repo, "test", "gravity.db.sql"), "utf8");
  const marker = sql.indexOf("vvv Test content following vvv");
  const rows = sql.slice(sql.indexOf("*/", marker) + 2)
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/^COMMIT;.*$/m, "");
  const sequences = ['"group"', "domainlist", "adlist", "client"]
    .map((table) => `SELECT setval(pg_get_serial_sequence('${table}', 'id'), (SELECT max(id) FROM ${table}));`)
    .join("\n");
  return `DELETE FROM info WHERE property IN ('gravity_count', 'antigravity_count', 'abp_domains', 'updated');
${rows}
${sequences}
UPDATE info SET value = (CAST(extract(epoch from now()) AS BIGINT) + 5)::text WHERE property = 'updated';`;
}

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
        assert.match(log, /Updating long-term database to version 23/);
      } else {
        assert.match(log, /Creating the long-term database \(version 23\)/);
      }
      assert.match(log, /Database successfully initialized/);
      assert.match(log, /Imported 0 queries from the (on-disk|long-term) database/);
      assert.deepEqual(errorLines(log), [], "lorentz.log contains ERROR lines");
    });

    it("leaves a healthy long-term database behind", async () => {
      await backend.healthy(lorentz);
      const db = lorentz.longterm;
      const version = Number(await db.query("SELECT value FROM lorentz WHERE id = 0;"));
      assert.ok(version >= 23, `database version ${version}`);
      const tables = (backend.name === "sqlite"
        ? await db.query("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;")
        : await db.query(`SELECT table_name FROM information_schema.tables WHERE table_schema = '${db.schema}' AND table_type = 'BASE TABLE' ORDER BY 1`)
      ).split("\n");
      for (const table of ["query_storage", "domain_by_id", "client_by_id", "forward_by_id",
        "addinfo_by_id", "message", "session", "network", "network_addresses", "aliasclient", "counters", "lorentz", "users"])
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

    if (backend.name === "postgres") it("reuses its connections to the server", async () => {
      // Every API request opens a connection to the long-term database. With the
      // pool they share a few sessions instead of opening one each
      const sessions = async () => {
        await sleep(1500); // the server publishes its statistics with a delay
        return Number(await lorentz.longterm.query("SELECT sessions FROM pg_stat_database WHERE datname = current_database()"));
      };
      await api(lorentz, "/api/info/messages/count");
      const before = await sessions();
      for (let i = 0; i < 40; i++) assert.equal((await api(lorentz, "/api/info/messages/count")).status, 200);
      const opened = (await sessions()) - before;
      assert.ok(opened < 10, `${opened} sessions opened for 40 requests`);
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
      if (backend.gravityOnServer)
        await lorentz.gravity.query(
          `INSERT INTO gravity(domain, adlist_id) VALUES ('${domain}', 1);
           UPDATE info SET value = (CAST(extract(epoch from now()) AS BIGINT) + 15)::text WHERE property = 'updated';`);
      else
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
      // A gravity database on a server is exported, but an import replaces
      // the tables of a file and skips it
      if (!backend.gravityOnServer)
        assert.ok(imported.body.files.some((file) => file.startsWith("etc/lorentz/gravity.db->")));

      // Lorentz restarts its DNS engine after an import
      await waitForApi(lorentz);
      await eventually("gravity to apply after the import",
        async () => (await dig(lorentz, GRAVITY_DOMAIN))[0] === "0.0.0.0", { timeoutMs: 45_000 });
      if (backend.gravityOnServer)
        assert.ok(Number(await lorentz.gravity.query("SELECT count(*) FROM gravity")) >= 8);
      else
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

// A web server for the adlists, in a container next to Lorentz (a container cannot
// reach the host on every machine). /slow.txt answers after four seconds. Lorentz
// reaches it as http://lists
const LIST_SERVER = `
import http.server, os, time
class H(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/slow.txt":
            time.sleep(4)
        if self.path == "/error":
            self.send_error(503)
            return
        super().do_GET()
os.chdir("/srv")
http.server.ThreadingHTTPServer(("", 80), H).serve_forever()
`;
async function startListServer(network) {
  const container = await new GenericContainer("python:3-alpine")
    .withNetwork(network)
    .withNetworkAliases("lists")
    .withCopyContentToContainer([{ content: LIST_SERVER, target: "/server.py" }])
    .withCommand(["python", "/server.py"])
    .withExposedPorts(80)
    .withWaitStrategy(Wait.forListeningPorts())
    .start();
  return {
    url: (path) => `http://lists${path}`,
    // Serve body at path
    async set(path, body) {
      const r = await container.exec(["sh", "-c", `printf '%s' "$1" > /srv${path}`, "sh", body]);
      assert.equal(r.exitCode, 0, r.output);
    },
    stop: () => container.stop(),
  };
}

const HOSTS_LIST = "# a list\n127.0.0.1 localhost\n0.0.0.0 tracker1.updater.example\n0.0.0.0 tracker2.updater.example\n0.0.0.0 shared.updater.example\n0.0.0.0 not_a_domain!!\n";
const ABP_LIST = "[Adblock Plus 2.0]\n! Title: abp\n||ads.abp.updater.example^\n##.banner\nplain.abp.updater.example\n";
const ALLOW_LIST = "shared.updater.example\n";

// The sample lists of the gravity database point at real addresses; a test of the
// updater has no business downloading them
async function disableSampleLists(lorentz) {
  const { body } = await api(lorentz, "/api/lists");
  for (const list of body.lists) {
    const path = `/api/lists/${encodeURIComponent(list.address)}?type=${list.type}`;
    const off = await api(lorentz, path, { method: "PUT", json: { comment: list.comment, groups: list.groups, enabled: false } });
    assert.equal(off.status, 200);
  }
}

const addList = (lorentz, address, type = "block") =>
  api(lorentz, `/api/lists?type=${type}`, { method: "POST", json: { address, groups: [0], enabled: true } });
const updateLists = (lorentz) => api(lorentz, "/api/action/gravity", { method: "POST" });
const listOf = async (lorentz, address) =>
  (await api(lorentz, "/api/lists")).body.lists.find((list) => list.address === address);

for (const backend of backends) describe(`adlist updater (${backend.name})`, () => {
  let lorentz, server, network;

  before(async () => {
    network = backend.name === "sqlite" ? await new Network().start() : postgres.network;
    server = await startListServer(network);
    await server.set("/hosts.txt", HOSTS_LIST);
    await server.set("/abp.txt", ABP_LIST);
    await server.set("/allow.txt", ALLOW_LIST);
    lorentz = await backend.start({}, { network });
    await disableSampleLists(lorentz);
  });
  after(async () => {
    await lorentz?.stop();
    await server?.stop();
    if (backend.name === "sqlite") await network?.stop();
  });

  it("downloads the lists and blocks the domains in them", async () => {
    assert.equal((await addList(lorentz, server.url("/hosts.txt"))).status, 201);
    assert.equal((await addList(lorentz, server.url("/abp.txt"))).status, 201);
    assert.equal((await addList(lorentz, server.url("/allow.txt"), "allow")).status, 201);
    assert.equal((await addList(lorentz, server.url("/missing.txt"))).status, 201);

    const result = await updateLists(lorentz);
    assert.equal(result.status, 200);
    assert.match(result.body, /Downloading 4 lists/);
    assert.match(result.body, /Done: 3 updated, 1 failed/);

    const hosts = await listOf(lorentz, server.url("/hosts.txt"));
    assert.equal(hosts.number, 3, "hosts, localhost being no domain to report");
    assert.equal(hosts.invalid_domains, 1);
    assert.equal(hosts.status, 1);
    assert.ok(hosts.date_updated > 0);
    const abp = await listOf(lorentz, server.url("/abp.txt"));
    assert.equal(abp.number, 2);
    assert.equal(abp.abp_entries, 1);
    const missing = await listOf(lorentz, server.url("/missing.txt"));
    assert.equal(missing.status, 3, "unavailable, and nothing to fall back on");

    const blocked = (domain) => async () => (await dig(lorentz, domain))[0] === "0.0.0.0";
    await eventually("the listed domain to be blocked", blocked("tracker1.updater.example"), { timeoutMs: 30_000 });
    await eventually("a subdomain of an ABP-style entry to be blocked", blocked("deep.ads.abp.updater.example"));
    await eventually("a plain domain of the ABP list to be blocked", blocked("plain.abp.updater.example"));
    assert.notDeepEqual(await dig(lorentz, "shared.updater.example"), BLOCKED, "an allow list wins");
    assert.notDeepEqual(await dig(lorentz, "unlisted.updater.example"), BLOCKED);
  });

  it("replaces the domains of a list that changed", async () => {
    await server.set("/hosts.txt", "0.0.0.0 tracker1.updater.example\n0.0.0.0 tracker3.updater.example\n");
    const result = await updateLists(lorentz);
    assert.match(result.body, /Done: 3 updated, 1 failed/);
    await eventually("the new domain to be blocked",
      async () => (await dig(lorentz, "tracker3.updater.example"))[0] === "0.0.0.0", { timeoutMs: 30_000 });
    await eventually("the removed domain to be let through",
      async () => (await dig(lorentz, "tracker2.updater.example"))[0] !== "0.0.0.0", { timeoutMs: 30_000 });
  });

  it("keeps the domains of a list whose download is not a list", async () => {
    await server.set("/hosts.txt", "<html><body>Service unavailable</body></html>");
    const result = await updateLists(lorentz);
    assert.match(result.body, /the download contains no domains \(keeping the domains it had\)/);
    const hosts = await listOf(lorentz, server.url("/hosts.txt"));
    assert.equal(hosts.status, 4, "unavailable, using what was downloaded before");
    assert.equal(hosts.number, 2);
    assert.equal((await dig(lorentz, "tracker3.updater.example"))[0], "0.0.0.0");

    assert.equal((await addList(lorentz, server.url("/error"))).status, 201);
    assert.match((await updateLists(lorentz)).body, /returned error: 503/);
    assert.equal((await dig(lorentz, "tracker3.updater.example"))[0], "0.0.0.0");
  });

  it("reads a list from a file", async () => {
    await run(lorentz, ["sh", "-c", "printf '0.0.0.0 fromfile.updater.example\\n' > /tmp/adlist.txt"]);
    assert.equal((await addList(lorentz, "file:///tmp/adlist.txt")).status, 201);
    assert.match((await updateLists(lorentz)).body, /file:\/\/\/tmp\/adlist.txt: 1 blocked domains/);
    await eventually("the domain of the file to be blocked",
      async () => (await dig(lorentz, "fromfile.updater.example"))[0] === "0.0.0.0", { timeoutMs: 30_000 });
  });

  it("turns a second update away while one is running", async () => {
    await server.set("/slow.txt", "0.0.0.0 slow.updater.example\n");
    assert.equal((await addList(lorentz, server.url("/slow.txt"))).status, 201);
    const first = updateLists(lorentz);
    await sleep(1500);
    const second = await updateLists(lorentz);
    assert.equal(second.status, 409);
    assert.equal(second.body.error.key, "conflict");
    assert.match((await first).body, /Done:/);
  });

  it("reports the databases it left behind", async () => {
    const { body } = await api(lorentz, "/api/info/lorentz");
    assert.ok(body.lorentz.database.gravity >= 4, `${body.lorentz.database.gravity} blocked domains`);
    assert.equal(body.lorentz.database.antigravity, 1);
  });
});

// The schedule needs a database that was never updated (the sample data are, at
// "updated" = 0) and about a minute after the start, so it is only done twice
for (const backend of backends.filter((b) => b.name !== "postgres")) describe(`scheduled adlist update (${backend.name})`, () => {
  let lorentz, server, network;

  before(async () => {
    network = backend.name === "sqlite" ? await new Network().start() : postgres.network;
    server = await startListServer(network);
    await server.set("/hosts.txt", HOSTS_LIST);
    lorentz = await backend.start({ LORENTZCONF_gravity_updateInterval: "1" }, { network });
    await disableSampleLists(lorentz);
    // Preparing the backend touched "updated"; the lists have to look old. Lorentz only
    // notices a newer value while it runs, so it reads this one at a restart
    const reset = "UPDATE info SET value = '0' WHERE property = 'updated'";
    if (lorentz.gravity) await lorentz.gravity.query(reset);
    else await sqlite(lorentz, GRAVITY_DB, `${reset};`);
    await restartLorentz(lorentz);
    assert.equal((await addList(lorentz, server.url("/hosts.txt"))).status, 201);
  });
  after(async () => {
    await lorentz?.stop();
    await server?.stop();
    if (backend.name === "sqlite") await network?.stop();
  });

  it("updates the lists by itself when they are older than the interval", async () => {
    await eventually("the scheduled update", async () =>
      /gravity: Done: 1 updated, 0 failed/.test(await lorentzLog(lorentz)), { timeoutMs: 180_000, intervalMs: 5000 })
      .catch(async (error) => {
        error.message += `\n${(await lorentzLog(lorentz)).split("\n").slice(-25).join("\n")}`;
        throw error;
      });
    const log = await lorentzLog(lorentz);
    assert.match(log, /gravity: The lists were never updated, updating them/);
    await eventually("the domain to be blocked",
      async () => (await dig(lorentz, "tracker1.updater.example"))[0] === "0.0.0.0", { timeoutMs: 30_000 });
  });

  it("does not update again after a restart while they are fresh", async () => {
    const count = async () => ((await lorentzLog(lorentz)).match(/gravity: Downloading/g) ?? []).length;
    const before = await count();
    await restartLorentz(lorentz);
    await sleep(100_000);
    assert.equal(await count(), before);
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

// Accounts: an open API becomes a closed one with the first account, and every
// account has the rights of its role
for (const backend of backends) describe(`user accounts (${backend.name})`, () => {
  let lorentz;
  const admin = { username: "admin", password: "admin-password-1" };
  const bootstrap = { ...admin, role: "admin", comment: "the first" };

  const login = async (username, password, extra = {}) => {
    const { status, body } = await api(lorentz, "/api/auth", { method: "POST", json: { username, password, ...extra } });
    return { status, body, sid: body?.session?.sid };
  };
  const as = (sid) => ({ headers: { sid } });
  const users = (sid, path = "", options = {}) => api(lorentz, `/api/users${path}`, { ...as(sid), ...options });
  let sid; // of the admin

  before(async () => {
    lorentz = await backend.start();
  });
  after(async () => {
    await lorentz?.stop();
  });

  it("is open until there is an account, and the first one has to be an admin", async () => {
    assert.equal((await api(lorentz, "/api/users")).status, 200);
    assert.deepEqual((await api(lorentz, "/api/users")).body.users, []);

    const viewer = await api(lorentz, "/api/users", { method: "POST", json: { ...bootstrap, role: "viewer" } });
    assert.equal(viewer.status, 409);
    const implicit = await api(lorentz, "/api/users", { method: "POST", json: { username: "x", password: "longenough" } });
    assert.equal(implicit.status, 409, "a viewer is the default, so this is refused too");

    const created = await api(lorentz, "/api/users", { method: "POST", json: bootstrap });
    assert.equal(created.status, 201);
    const [user] = created.body.users;
    assert.equal(user.username, "admin");
    assert.equal(user.role, "admin");
    assert.equal(user.enabled, true);
    assert.equal(user.comment, "the first");
    assert.equal(user.last_login, null);
    assert.ok(user.id > 0 && user.created_at > 0);
    assert.equal(user.pwhash, undefined, "the hash is never returned");
    assert.equal(JSON.stringify(created.body).includes("BALLOON"), false);
  });

  it("requires a login from then on", async () => {
    assert.equal((await api(lorentz, "/api/users")).status, 401);
    assert.equal((await api(lorentz, "/api/stats/summary")).status, 401);
    assert.equal((await login("admin", "wrong")).status, 401);
    assert.equal((await login("nobody", admin.password)).status, 401);
    assert.equal((await login("admin", "")).status, 401);
    // The password of the configuration is empty, it does not open the API
    assert.equal((await api(lorentz, "/api/auth", { method: "POST", json: { password: "" } })).status, 401);

    const ok = await login("ADMIN", admin.password); // names are not case sensitive
    assert.equal(ok.status, 200);
    assert.equal(ok.body.session.valid, true);
    assert.equal(ok.body.session.user.username, "admin");
    assert.equal(ok.body.session.user.role, "admin");
    sid = ok.sid;
    assert.equal((await api(lorentz, "/api/stats/summary", as(sid))).status, 200);
  });

  it("lists, creates, reads and validates accounts", async () => {
    for (const [body, key] of [
      [{ username: "", password: "longenough" }, "empty name"],
      [{ username: "has space", password: "longenough" }, "name with a space"],
      [{ username: "a".repeat(65), password: "longenough" }, "name too long"],
      [{ username: "bob", password: "short" }, "password too short"],
      [{ username: "bob" }, "no password"],
      [{ password: "longenough" }, "no name"],
      [{ username: 5, password: "longenough" }, "name not a string"],
      [{ username: "bob", password: "longenough", role: "root" }, "unknown role"],
      [{ username: "bob", password: "longenough", enabled: "yes" }, "enabled not a boolean"],
    ]) {
      const { status, body: error } = await users(sid, "", { method: "POST", json: body });
      assert.equal(status, 400, key);
      assert.equal(error.error.key, "bad_request", key);
    }
    assert.equal((await users(sid, "", { method: "POST", json: { username: "admin", password: "longenough" } })).status, 409);
    assert.equal((await users(sid, "", { method: "POST", json: { username: "ADMIN", password: "longenough" } })).status, 409);

    const bob = await users(sid, "", { method: "POST", json: { username: "Bob.Smith@home", password: "bobs-password", comment: "reads" } });
    assert.equal(bob.status, 201);
    assert.equal(bob.body.users[0].username, "bob.smith@home");
    assert.equal(bob.body.users[0].role, "viewer", "the default role");

    const list = await users(sid);
    assert.deepEqual(list.body.users.map((user) => user.username), ["admin", "bob.smith@home"]);
    const one = await users(sid, "/BOB.smith@home");
    assert.equal(one.body.users[0].comment, "reads");
    assert.equal((await users(sid, "/nobody")).status, 404);
    assert.equal((await users(sid, "/nobody", { method: "PUT", json: { comment: "x" } })).status, 404);
    assert.equal((await users(sid, "/nobody", { method: "DELETE" })).status, 404);

    // The database has what the API says, and not the password
    const stored = await lorentz.longterm.query("SELECT username || ':' || role FROM users ORDER BY id");
    assert.equal(stored, "admin:admin\nbob.smith@home:viewer");
    assert.equal(await lorentz.longterm.query("SELECT count(*) FROM users WHERE pwhash LIKE '%bobs-password%'"), "0");
  });

  it("gives a viewer the rights to look and nothing more", async () => {
    const viewer = await login("bob.smith@home", "bobs-password");
    assert.equal(viewer.status, 200);
    assert.equal(viewer.body.session.user.role, "viewer");
    const v = viewer.sid;

    assert.equal((await api(lorentz, "/api/stats/summary", as(v))).status, 200);
    assert.equal((await api(lorentz, "/api/queries?length=1", as(v))).status, 200);
    assert.equal((await api(lorentz, "/api/domains", as(v))).status, 200);
    // Nothing that changes, nothing that holds secrets
    const write = await api(lorentz, "/api/domains/deny/exact", { ...as(v), method: "POST", json: { domain: "x.example", groups: [0] } });
    assert.equal(write.status, 403);
    assert.equal(write.body.error.key, "forbidden");
    assert.equal((await api(lorentz, "/api/groups", { ...as(v), method: "POST", json: { name: "g" } })).status, 403);
    assert.equal((await api(lorentz, "/api/action/restartdns", { ...as(v), method: "POST" })).status, 403);
    for (const path of ["/api/config", "/api/teleporter", "/api/logs/lorentz", "/api/auth/sessions", "/api/auth/app"])
      assert.equal((await api(lorentz, path, as(v))).status, 403, path);

    // Accounts: not the others, not to create, but themselves
    assert.equal((await users(v)).status, 403);
    assert.equal((await users(v, "", { method: "POST", json: { username: "eve", password: "longenough", role: "admin" } })).status, 403);
    assert.equal((await users(v, "/admin")).status, 403);
    assert.equal((await users(v, "/admin", { method: "PUT", json: { comment: "hacked" } })).status, 403);
    assert.equal((await users(v, "/admin", { method: "DELETE" })).status, 403);
    assert.equal((await users(v, "/bob.smith@home")).status, 200);
    for (const change of [{ role: "admin" }, { enabled: false }, { username: "bobby" }])
      assert.equal((await users(v, "/bob.smith@home", { method: "PUT", json: change })).status, 403, JSON.stringify(change));
    assert.equal((await users(v, "/bob.smith@home", { method: "DELETE" })).status, 403);
    assert.equal((await users(v, "/bob.smith@home", { method: "PUT", json: { comment: "my own" } })).status, 200);

    // The session says who they are, and lists the accounts of the others for an admin
    const sessions = await api(lorentz, "/api/auth/sessions", as(sid));
    assert.deepEqual(sessions.body.sessions.map((session) => session.user?.username).sort(), ["admin", "bob.smith@home"]);
    assert.equal((await api(lorentz, "/api/auth", { method: "DELETE", ...as(v) })).status, 204);
    assert.equal((await api(lorentz, "/api/stats/summary", as(v))).status, 401);
  });

  it("changes a password with the old one, and ends the other sessions", async () => {
    const first = await login("bob.smith@home", "bobs-password");
    const second = await login("bob.smith@home", "bobs-password");
    const [a, b] = [first.sid, second.sid];

    const path = "/bob.smith@home";
    assert.equal((await users(a, path, { method: "PUT", json: { password: "the-new-password" } })).status, 400, "no current password");
    assert.equal((await users(a, path, { method: "PUT", json: { password: "the-new-password", current_password: "wrong" } })).status, 401);
    assert.equal((await users(a, path, { method: "PUT", json: { password: "short", current_password: "bobs-password" } })).status, 400);
    const changed = await users(a, path, { method: "PUT", json: { password: "the-new-password", current_password: "bobs-password" } });
    assert.equal(changed.status, 200);

    assert.equal((await api(lorentz, "/api/stats/summary", as(a))).status, 200, "the session that asked goes on");
    assert.equal((await api(lorentz, "/api/stats/summary", as(b))).status, 401, "the other one ended");
    assert.equal((await login("bob.smith@home", "bobs-password")).status, 401);
    const again = await login("bob.smith@home", "the-new-password");
    assert.equal(again.status, 200);

    // An admin resets it without knowing the old one
    const reset = await users(sid, path, { method: "PUT", json: { password: "reset-by-admin" } });
    assert.equal(reset.status, 200);
    assert.equal((await api(lorentz, "/api/stats/summary", as(again.sid))).status, 401, "sessions end on a reset");
    assert.equal((await login("bob.smith@home", "reset-by-admin")).status, 200);
    assert.notEqual(reset.body.users[0].last_login, undefined);
  });

  it("disables, enables, renames and deletes", async () => {
    const path = "/bob.smith@home";
    const session = await login("bob.smith@home", "reset-by-admin");
    assert.equal((await api(lorentz, "/api/stats/summary", as(session.sid))).status, 200);

    const disabled = await users(sid, path, { method: "PUT", json: { enabled: false } });
    assert.equal(disabled.status, 200);
    assert.equal(disabled.body.users[0].enabled, false);
    assert.equal((await api(lorentz, "/api/stats/summary", as(session.sid))).status, 401, "its session ends");
    assert.equal((await login("bob.smith@home", "reset-by-admin")).status, 401, "and it cannot log in");

    assert.equal((await users(sid, path, { method: "PUT", json: { enabled: true, role: "admin", comment: null } })).status, 200);
    const promoted = await login("bob.smith@home", "reset-by-admin");
    assert.equal(promoted.status, 200);
    assert.equal(promoted.body.session.user.role, "admin");
    assert.equal((await users(promoted.sid)).status, 200, "an admin manages accounts");
    // The role applies at once, without a new login
    assert.equal((await users(sid, path, { method: "PUT", json: { role: "viewer" } })).status, 200);
    assert.equal((await users(promoted.sid)).status, 403);
    assert.equal((await api(lorentz, "/api/stats/summary", as(promoted.sid))).status, 200);

    const renamed = await users(sid, path, { method: "PUT", json: { username: "Robert" } });
    assert.equal(renamed.status, 200);
    assert.equal(renamed.body.users[0].username, "robert");
    assert.equal((await users(sid, path)).status, 404);
    assert.equal((await users(sid, "/robert", { method: "PUT", json: { username: "admin" } })).status, 409);
    assert.equal((await api(lorentz, "/api/stats/summary", as(promoted.sid))).status, 200, "a rename keeps the session");
    assert.equal((await login("robert", "reset-by-admin")).status, 200);

    assert.equal((await users(sid, "/robert", { method: "DELETE" })).status, 204);
    assert.equal((await api(lorentz, "/api/stats/summary", as(promoted.sid))).status, 401, "a deleted account has no session");
    assert.equal((await login("robert", "reset-by-admin")).status, 401);
    assert.equal((await users(sid, "/robert")).status, 404);
  });

  it("never leaves the API without an enabled admin", async () => {
    const own = await users(sid, "/admin", { method: "DELETE" });
    assert.equal(own.status, 409, "your own account");
    assert.equal((await users(sid, "/admin", { method: "PUT", json: { enabled: false } })).status, 409);
    assert.equal((await users(sid, "/admin", { method: "PUT", json: { role: "viewer" } })).status, 409);

    // A second admin can remove the first, but not the last
    assert.equal((await users(sid, "", { method: "POST", json: { username: "second", password: "second-password", role: "admin" } })).status, 201);
    const second = await login("second", "second-password");
    assert.equal((await users(second.sid, "/admin", { method: "DELETE" })).status, 204);
    assert.equal((await users(second.sid, "/second", { method: "DELETE" })).status, 409);
    assert.equal((await users(second.sid, "/second", { method: "PUT", json: { role: "viewer" } })).status, 409);
    assert.equal((await users(second.sid, "/second", { method: "PUT", json: { comment: "still fine" } })).status, 200);
    sid = second.sid;
  });

  it("keeps the accounts and their sessions across a restart", async () => {
    assert.equal((await users(sid, "", { method: "POST", json: { username: "kept", password: "kept-password", role: "viewer" } })).status, 201);
    const kept = await login("kept", "kept-password");
    await restartLorentz(lorentz);

    assert.match(await lorentzLog(lorentz), /Restored \d+ API sessions?/);
    assert.equal((await api(lorentz, "/api/stats/summary", as(kept.sid))).status, 200);
    const check = await api(lorentz, "/api/auth", as(kept.sid));
    assert.equal(check.body.session.user.username, "kept");
    assert.equal((await users(sid)).body.users.length, 2);
    assert.equal((await login("kept", "kept-password")).status, 200);
    assert.equal((await api(lorentz, "/api/users")).status, 401);
    assert.deepEqual(errorLines(await lorentzLog(lorentz)), []);
  });
});

// With a password in the configuration as well, both ways in work
for (const backend of backends) describe(`accounts and the password of the configuration (${backend.name})`, () => {
  let lorentz;
  const password = "configured-password";

  before(async () => {
    lorentz = await backend.start({ LORENTZCONF_webserver_api_password: password });
  });
  after(async () => {
    await lorentz?.stop();
  });

  it("creates an account through a login with the configured password", async () => {
    assert.equal((await api(lorentz, "/api/users")).status, 401);
    const configured = await api(lorentz, "/api/auth", { method: "POST", json: { password } });
    assert.equal(configured.status, 200);
    assert.equal(configured.body.session.user, null, "not an account");
    const sid = configured.body.session.sid;

    // Its rights are the ones of an admin, and here the first account may be a viewer
    const created = await api(lorentz, "/api/users", { method: "POST", headers: { sid }, json: { username: "carol", password: "carols-password" } });
    assert.equal(created.status, 201);
    assert.equal(created.body.users[0].role, "viewer");
    assert.equal((await api(lorentz, "/api/users/carol", { headers: { sid } })).status, 200);

    const carol = await api(lorentz, "/api/auth", { method: "POST", json: { username: "carol", password: "carols-password" } });
    assert.equal(carol.status, 200);
    assert.equal((await api(lorentz, "/api/users", { headers: { sid: carol.body.session.sid } })).status, 403);
    // The password of the configuration does not log in to an account and the other way round
    assert.equal((await api(lorentz, "/api/auth", { method: "POST", json: { username: "carol", password } })).status, 401);
    assert.equal((await api(lorentz, "/api/auth", { method: "POST", json: { password: "carols-password" } })).status, 401);
    // Deleting the last account is fine when it is no admin: the configured password is still the way in
    assert.equal((await api(lorentz, "/api/users/carol", { method: "DELETE", headers: { sid } })).status, 204);
    assert.equal((await api(lorentz, "/api/users", { headers: { sid: carol.body.session.sid } })).status, 401);
    assert.equal((await api(lorentz, "/api/stats/summary", { headers: { sid } })).status, 200);
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
    assert.match(log, /Updating long-term database to version 23/);
    assert.match(log, /Database successfully initialized/);
    assert.deepEqual(errorLines(log), []);
    assert.ok(Number(await sqlite(lorentz, LORENTZ_DB, "SELECT value FROM lorentz WHERE id = 0;")) >= 23);
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

// The connection data can come from the environment instead of the URI: libpq reads
// PGHOST, PGUSER, PGPASSWORD, PGDATABASE, PGOPTIONS and the rest of its variables
// for whatever the URI leaves out, so no password has to be written to lorentz.toml
if (wanted.includes("postgres")) describe("PostgreSQL connection data from the environment", () => {
  let lorentz;
  let schema;

  before(async () => {
    schema = await postgres.schema();
    lorentz = await startLorentz({
      LORENTZCONF_files_database: "postgresql://",
      PGHOST: "pg",
      PGUSER: "postgres",
      PGPASSWORD: "lorentz",
      PGDATABASE: "lorentz_test",
      PGOPTIONS: `-c search_path=${schema.name}`,
    }, { network: postgres.network });
  });
  after(async () => {
    await lorentz?.stop();
  });

  it("connects and creates its tables", async () => {
    const log = await lorentzLog(lorentz);
    assert.match(log, /Creating the long-term database \(version 23\)/);
    assert.match(log, /Database successfully initialized/);
    assert.deepEqual(errorLines(log), []);
    assert.equal(await schema.sql("SELECT value FROM lorentz WHERE id = 0"), "23");
  });

  it("keeps the password out of the configuration and the log", async () => {
    const toml = await run(lorentz, ["cat", "/etc/lorentz/lorentz.toml"]);
    assert.match(toml, /database = "postgresql:\/\/"/);
    assert.doesNotMatch(toml, /PGPASSWORD|lorentz_test/);
    assert.doesNotMatch(await lorentzLog(lorentz), /PGPASSWORD|:lorentz@/);
  });

  it("works", async () => {
    assert.deepEqual(await dig(lorentz, LOCAL_DOMAIN), ["1.2.3.4"]);
    await eventually("the query in the query log", async () => {
      const { body } = await api(lorentz, `/api/queries?domain=${LOCAL_DOMAIN}`);
      return body.queries.length >= 1;
    });
    await restartLorentz(lorentz);
    assert.deepEqual(errorLines(await lorentzLog(lorentz)), []);
    assert.ok(Number(await schema.sql("SELECT count(*) FROM query_storage")) >= 1);
  });
});
