// Integration tests for pihole-FTL. They run the real binary in a container
// and check, from the outside, that FTL behaves normally: it starts and
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
// (build FTL first, see FTL_BINARY in lib.mjs). Needs a running Docker daemon.
import { after, before, describe, it } from "node:test";
import assert from "node:assert/strict";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import {
  FTL_DB, GRAVITY_DB, api, apiBinary, buildImage, dig, eventually, ftlLog,
  restartFtl, run, settle, sleep, sqlite, startFtl, waitForApi,
} from "./lib.mjs";

const BLOCKED = ["0.0.0.0"];

// Names in the gravity database of the sample data (test/gravity.db.sql)
const GRAVITY_DOMAIN = "gravity.ftl"; // in the gravity table
const DENIED_DOMAIN = "denied.ftl"; // exact denylist
const ALLOWED_DOMAIN = "allowed.ftl"; // exact allowlist
const LOCAL_DOMAIN = "local.example.com"; // local record set through dns.hosts

// SQLite reports "recovered N frames from WAL file" (code 283, a notice) after
// FTL restarted itself, e.g. following a Teleporter import, and FTL logs SQLite
// messages at ERROR level. It is expected there and says nothing about the
// database, everything else at ERROR level is a problem
const isWalRecovery = (line) => /recovered \d+ frames from WAL file/.test(line);
const errorLines = (log) => log.split("\n").filter((line) => /\bERROR\b/.test(line) && !isWalRecovery(line));

before(async () => {
  await buildImage();
});

describe("a fresh pihole-FTL", () => {
  let ftl;

  before(async () => {
    ftl = await startFtl();
  });
  after(async () => {
    await ftl?.stop();
  });

  describe("startup", () => {
    it("migrates the long-term database from scratch and reports no errors", async () => {
      const log = await ftlLog(ftl);
      assert.match(log, /Database version is 1\b/);
      assert.match(log, /Updating long-term database to version 22/);
      assert.match(log, /Database successfully initialized/);
      assert.match(log, /Imported 0 queries from the long-term database/);
      assert.deepEqual(errorLines(log), [], "FTL.log contains ERROR lines");
    });

    it("leaves a healthy long-term database behind", async () => {
      assert.equal(await sqlite(ftl, FTL_DB, "PRAGMA integrity_check;"), "ok");
      const version = Number(await sqlite(ftl, FTL_DB, "SELECT value FROM ftl WHERE id = 0;"));
      assert.ok(version >= 22, `database version ${version}`);
      const tables = (await sqlite(ftl, FTL_DB,
        "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name;")).split("\n");
      for (const table of ["query_storage", "domain_by_id", "client_by_id", "forward_by_id",
        "addinfo_by_id", "message", "session", "network", "network_addresses", "aliasclient", "counters", "ftl"])
        assert.ok(tables.includes(table), `table ${table} exists`);
      assert.equal(await sqlite(ftl, FTL_DB,
        "SELECT count(*) FROM sqlite_master WHERE type = 'view' AND name = 'queries';"), "1");
    });

    it("reads the gravity database", async () => {
      const { status, body } = await api(ftl, "/api/info/ftl");
      assert.equal(status, 200);
      assert.equal(body.ftl.database.gravity, 8);
      assert.equal(body.ftl.database.antigravity, 2);
      assert.ok(body.ftl.database.groups >= 1);
      assert.ok(body.ftl.database.clients >= 1);
      assert.equal(body.ftl.database.domains.denied.total, 2);
    });

    it("reports the SQLite version", async () => {
      const { status, body } = await api(ftl, "/api/info/version");
      assert.equal(status, 200);
      assert.match(body.version.ftl.local.version, /\S/);
      const sqliteVersion = await run(ftl, ["pihole-FTL", "sqlite3", "-batch", ":memory:", "SELECT sqlite_version();"]);
      assert.match(sqliteVersion, /^3\.\d+\.\d+/);
    });
  });

  describe("DNS", () => {
    it("answers from local records", async () => {
      assert.deepEqual(await dig(ftl, LOCAL_DOMAIN), ["1.2.3.4"]);
    });

    it("blocks a domain from gravity", async () => {
      assert.deepEqual(await dig(ftl, GRAVITY_DOMAIN), BLOCKED);
    });

    it("blocks a domain on the exact denylist", async () => {
      assert.deepEqual(await dig(ftl, DENIED_DOMAIN), BLOCKED);
    });

    it("does not block a domain on the exact allowlist", async () => {
      assert.notDeepEqual(await dig(ftl, ALLOWED_DOMAIN), BLOCKED);
    });

    it("records the decisions in the query log", async () => {
      const result = await eventually("queries in the query log", async () => {
        const { body } = await api(ftl, "/api/queries?length=100");
        return body.queries.length >= 4 ? body : null;
      });
      const status = (domain) => result.queries.find((q) => q.domain === domain)?.status;
      assert.equal(status(GRAVITY_DOMAIN), "GRAVITY");
      assert.equal(status(DENIED_DOMAIN), "DENYLIST");
      assert.ok(status(LOCAL_DOMAIN), "local record was logged");
      assert.ok(status(ALLOWED_DOMAIN), "allowed domain was logged");
    });

    it("filters the query log", async () => {
      const gravity = await api(ftl, `/api/queries?domain=${GRAVITY_DOMAIN}`);
      assert.equal(gravity.body.queries.length, 1);
      assert.equal(gravity.body.queries[0].client.ip, "127.0.0.1");

      const denied = await api(ftl, "/api/queries?status=DENYLIST");
      assert.ok(denied.body.queries.every((q) => q.status === "DENYLIST"));
      assert.equal(denied.body.queries[0].domain, DENIED_DOMAIN);

      const limited = await api(ftl, "/api/queries?length=1");
      assert.equal(limited.body.queries.length, 1);

      const none = await api(ftl, "/api/queries?domain=nosuchdomain.invalid");
      assert.equal(none.body.queries.length, 0);
    });

    it("summarizes the traffic", async () => {
      const { status, body } = await api(ftl, "/api/stats/summary");
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
      assert.notDeepEqual(await dig(ftl, domain), BLOCKED);

      const created = await api(ftl, "/api/domains/deny/exact", {
        method: "POST", json: { domain, comment: "integration test", groups: [0], enabled: true },
      });
      assert.equal(created.status, 201);
      assert.equal(created.body.processed.errors.length, 0);

      const read = await api(ftl, `/api/domains/deny/exact/${domain}`);
      assert.equal(read.body.domains[0].domain, domain);
      assert.equal(read.body.domains[0].comment, "integration test");
      assert.deepEqual(read.body.domains[0].groups, [0]);

      await eventually("the new entry to block", async () => (await dig(ftl, domain))[0] === "0.0.0.0");

      const removed = await api(ftl, `/api/domains/deny/exact/${domain}`, { method: "DELETE" });
      assert.equal(removed.status, 204);
      await eventually("the entry to be gone", async () => (await dig(ftl, domain))[0] !== "0.0.0.0");
    });

    it("rejects an invalid regular expression", async () => {
      const { status, body } = await api(ftl, "/api/domains/deny/regex", {
        method: "POST", json: { domain: "(", groups: [0], enabled: true },
      });
      assert.equal(status, 400);
      assert.equal(body.error.key, "regex_error");
    });

    it("resolves a client's groups when its configuration changes", async () => {
      // The sample data configures 127.0.0.1, the client of these tests. Move
      // it into a group that is on no list: gravity no longer applies to it
      const original = (await api(ftl, "/api/clients/127.0.0.1")).body.clients[0];
      const group = await api(ftl, "/api/groups", {
        method: "POST", json: { name: "not-blocked", comment: "integration test", enabled: true },
      });
      assert.equal(group.status, 201);
      const groupId = (await api(ftl, "/api/groups/not-blocked")).body.groups[0].id;

      const moved = await api(ftl, "/api/clients/127.0.0.1", {
        method: "PUT", json: { comment: original.comment, groups: [groupId] },
      });
      assert.equal(moved.status, 200);
      await eventually("gravity to stop applying to the client",
        async () => (await dig(ftl, GRAVITY_DOMAIN))[0] !== "0.0.0.0", { timeoutMs: 30_000 });

      const restored = await api(ftl, "/api/clients/127.0.0.1", {
        method: "PUT", json: { comment: original.comment, groups: original.groups },
      });
      assert.equal(restored.status, 200);
      await eventually("gravity to apply again",
        async () => (await dig(ftl, GRAVITY_DOMAIN))[0] === "0.0.0.0", { timeoutMs: 30_000 });
      assert.equal((await api(ftl, "/api/groups/not-blocked", { method: "DELETE" })).status, 204);
    });

    it("picks up a rebuilt gravity database", async () => {
      const domain = "rebuilt.example";
      assert.notDeepEqual(await dig(ftl, domain), BLOCKED);
      await settle(ftl); // no reload may be running while gravity.db is written
      await sqlite(ftl, GRAVITY_DB,
        `INSERT INTO gravity(domain, adlist_id) VALUES ('${domain}', 1);
         UPDATE info SET value = CAST(strftime('%s','now') AS INTEGER) + 5 WHERE property = 'updated';`);
      await eventually("FTL to notice the new gravity",
        async () => (await dig(ftl, domain))[0] === "0.0.0.0", { timeoutMs: 30_000 });
      await eventually("the reload to be logged",
        async () => /Gravity database has been updated, reloading now/.test(await ftlLog(ftl)));
    });
  });

  describe("network table and messages", () => {
    it("records the client as a network device", async () => {
      const devices = await eventually("the client in the network table", async () => {
        const { body } = await api(ftl, "/api/network/devices");
        return body.devices?.length ? body.devices : null;
      }, { timeoutMs: 30_000 });
      const device = devices.find((d) => d.ips.some((ip) => ip.ip === "127.0.0.1"));
      assert.ok(device, "127.0.0.1 is a known device");
      assert.ok(device.numQueries > 0);
      assert.equal(device.interface, "lo");
    });

    it("lists and deletes messages", async () => {
      const list = await api(ftl, "/api/info/messages");
      assert.equal(list.status, 200);
      const count = await api(ftl, "/api/info/messages/count");
      assert.equal(count.body.count, list.body.messages.length);
      assert.ok(list.body.messages.length >= 1, "FTL warns about the missing upstream servers");
      const [message] = list.body.messages;
      assert.ok(message.plain && message.html);

      const removed = await api(ftl, `/api/info/messages/${message.id}`, { method: "DELETE" });
      assert.equal(removed.status, 204);
      const after = await api(ftl, "/api/info/messages/count");
      assert.equal(after.body.count, count.body.count - 1);
    });
  });

  describe("Teleporter", () => {
    it("exports an archive and imports it again", async () => {
      const archive = await apiBinary(ftl, "/api/teleporter");
      assert.equal(archive.status, 200);
      assert.equal(archive.data.subarray(0, 2).toString(), "PK", "the archive is a ZIP file");
      assert.ok(archive.data.length > 1000);

      const form = new FormData();
      form.append("file", new Blob([archive.data]), "teleporter.zip");
      const imported = await api(ftl, "/api/teleporter", { method: "POST", body: form });
      assert.equal(imported.status, 200);
      assert.ok(imported.body.files.some((file) => file.startsWith("etc/pihole/gravity.db->")));

      // FTL restarts its DNS engine after an import
      await waitForApi(ftl);
      await eventually("gravity to apply after the import",
        async () => (await dig(ftl, GRAVITY_DOMAIN))[0] === "0.0.0.0", { timeoutMs: 45_000 });
      assert.equal(await sqlite(ftl, GRAVITY_DB, "PRAGMA integrity_check;"), "ok");
    });
  });

  describe("restart", () => {
    it("keeps the query history in the long-term database", async () => {
      // Traffic in a known amount, then the totals before the restart
      for (const domain of [GRAVITY_DOMAIN, DENIED_DOMAIN, LOCAL_DOMAIN, ALLOWED_DOMAIN])
        await dig(ftl, domain);
      const before = await eventually("all queries logged", async () => {
        const { body } = await api(ftl, "/api/queries?length=1");
        return body.recordsTotal >= 8 ? body : null;
      });

      await restartFtl(ftl);

      // The shutdown exports everything, the start imports it again
      const log = await ftlLog(ftl);
      assert.match(log, /Imported \d+ queries from the on-disk database/);
      const stored = Number(await sqlite(ftl, FTL_DB, "SELECT count(*) FROM query_storage;"));
      assert.ok(stored >= before.recordsTotal, `${stored} stored, ${before.recordsTotal} expected`);

      const after = await api(ftl, "/api/queries?length=1");
      assert.ok(after.body.recordsTotal >= before.recordsTotal, "history survived the restart");
      const gravity = await api(ftl, `/api/queries?domain=${GRAVITY_DOMAIN}`);
      assert.ok(gravity.body.queries.length >= 1);
      assert.equal(gravity.body.queries[0].status, "GRAVITY");

      // And FTL keeps working
      assert.deepEqual(await dig(ftl, GRAVITY_DOMAIN), BLOCKED);
      assert.equal(await sqlite(ftl, FTL_DB, "PRAGMA integrity_check;"), "ok");
    });

    it("serves statistics from the long-term database", async () => {
      const now = Math.floor(Date.now() / 1000);
      const window = `from=${now - 3600}&until=${now + 3600}`;

      const summary = await api(ftl, `/api/stats/database/summary?${window}`);
      assert.equal(summary.status, 200);
      assert.ok(summary.body.sum_queries >= 8);
      assert.ok(summary.body.sum_blocked >= 2);
      assert.ok(summary.body.total_clients >= 1);

      const domains = await api(ftl, `/api/stats/database/top_domains?${window}`);
      assert.ok(domains.body.domains.some((d) => d.domain === LOCAL_DOMAIN));
      const blocked = await api(ftl, `/api/stats/database/top_domains?${window}&blocked=true`);
      assert.ok(blocked.body.domains.some((d) => d.domain === GRAVITY_DOMAIN));
      const clients = await api(ftl, `/api/stats/database/top_clients?${window}`);
      assert.equal(clients.body.clients[0].ip, "127.0.0.1");

      const history = await api(ftl, `/api/history/database?${window}`);
      assert.ok(history.body.history.length >= 1);
      assert.ok(history.body.history.reduce((sum, bucket) => sum + bucket.total, 0) >= 8);
      const clientHistory = await api(ftl, `/api/history/database/clients?${window}`);
      assert.ok(Object.keys(clientHistory.body.clients).includes("127.0.0.1"));
      const types = await api(ftl, `/api/stats/database/query_types?${window}`);
      assert.equal(types.status, 200);
      assert.ok(Object.keys(types.body.types).includes("A"));
      const upstreams = await api(ftl, `/api/stats/database/upstreams?${window}`);
      assert.equal(upstreams.status, 200);
    });

    it("shuts down without database errors", async () => {
      await restartFtl(ftl);
      const log = await ftlLog(ftl);
      assert.deepEqual(errorLines(log), [], "FTL.log contains ERROR lines");
      assert.doesNotMatch(log, /Statement not finalized/);
      assert.doesNotMatch(log, /SQL error/i);
      assert.doesNotMatch(log, /is damaged/);
      assert.doesNotMatch(log, /is read-only/);
    });
  });
});

describe("API sessions", () => {
  let ftl;
  const password = "integration-test-password";

  before(async () => {
    ftl = await startFtl({ FTLCONF_webserver_api_password: password });
  });
  after(async () => {
    await ftl?.stop();
  });

  it("rejects a request without a session", async () => {
    const { status } = await api(ftl, "/api/config/dns/upstreams");
    assert.equal(status, 401);
  });

  it("logs in and keeps the session across a restart", async () => {
    const wrong = await api(ftl, "/api/auth", { method: "POST", json: { password: "wrong" } });
    assert.equal(wrong.status, 401);

    const login = await api(ftl, "/api/auth", { method: "POST", json: { password } });
    assert.equal(login.status, 200);
    assert.equal(login.body.session.valid, true);
    const { sid } = login.body.session;
    assert.ok(sid.length > 10);
    assert.equal((await api(ftl, "/api/config/dns/upstreams", { headers: { sid } })).status, 200);

    // The sessions are stored in the long-term database on shutdown
    await restartFtl(ftl);
    assert.match(await ftlLog(ftl), /Restored 1 API session/);
    const check = await api(ftl, "/api/auth", { headers: { sid } });
    assert.equal(check.status, 200);
    assert.equal(check.body.session.valid, true);
    assert.equal((await api(ftl, "/api/config/dns/upstreams", { headers: { sid } })).status, 200);

    // Logging out ends it for good
    assert.equal((await api(ftl, "/api/auth", { method: "DELETE", headers: { sid } })).status, 204);
    assert.equal((await api(ftl, "/api/config/dns/upstreams", { headers: { sid } })).status, 401);
  });
});

describe("upgrading a database", () => {
  let ftl;
  const fixture = join(dirname(fileURLToPath(import.meta.url)), "..", "pihole-FTL.db.sql");

  // The long-term database the bats suite starts from is a version 9 database
  // with real rows. Rebuild it before FTL starts, then let FTL migrate it
  before(async () => {
    ftl = await startFtl({}, {
      files: [{ source: fixture, target: "/tmp/pihole-FTL.db.sql" }],
      command: ["sh", "-c",
        "pihole-FTL sqlite3 /etc/pihole/pihole-FTL.db < /tmp/pihole-FTL.db.sql && exec pihole-FTL no-daemon"],
    });
  });
  after(async () => {
    await ftl?.stop();
  });

  it("migrates every version up to the current one", async () => {
    const log = await ftlLog(ftl);
    assert.match(log, /Database version is 9\b/);
    assert.match(log, /Updating long-term database to version 10/);
    assert.match(log, /Updating long-term database to version 22/);
    assert.match(log, /Database successfully initialized/);
    assert.deepEqual(errorLines(log), []);
    assert.ok(Number(await sqlite(ftl, FTL_DB, "SELECT value FROM ftl WHERE id = 0;")) >= 22);
    assert.equal(await sqlite(ftl, FTL_DB, "PRAGMA integrity_check;"), "ok");
  });

  it("keeps the network table and the alias-clients", async () => {
    const { status, body } = await api(ftl, "/api/network/devices");
    assert.equal(status, 200);
    const device = body.devices.find((d) => d.hwaddr === "aa:bb:cc:dd:ee:ff");
    assert.ok(device, "the device of the old database is still there");
    assert.equal(device.interface, "lo123");
    assert.deepEqual(device.ips.map((ip) => ip.ip).sort(), ["127.0.0.4", "127.0.0.5"]);
    const other = body.devices.find((d) => d.hwaddr === "00:11:22:33:44:55");
    assert.deepEqual(other.ips.map((ip) => ip.ip), ["127.0.0.6"]);
    assert.equal(await sqlite(ftl, FTL_DB, "SELECT name FROM aliasclient WHERE id = 0;"), "some-aliasclient");
    // The old query table became a view over the new storage tables
    assert.equal(await sqlite(ftl, FTL_DB, "SELECT count(*) FROM queries;"),
      await sqlite(ftl, FTL_DB, "SELECT count(*) FROM query_storage;"));
  });

  it("works after the migration", async () => {
    assert.deepEqual(await dig(ftl, GRAVITY_DOMAIN), BLOCKED);
    assert.deepEqual(await dig(ftl, LOCAL_DOMAIN), ["1.2.3.4"]);
    const { status } = await api(ftl, "/api/stats/summary");
    assert.equal(status, 200);
    const stats = await api(ftl, "/api/stats/database/summary?from=1&until=2000000000");
    assert.equal(stats.status, 200);
  });
});
