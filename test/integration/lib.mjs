// Helpers of the lorentz integration tests: image build, container start,
// and thin wrappers around the API, DNS and the SQLite shell of the binary
// under test.
import { existsSync, mkdtempSync, copyFileSync, rmSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { GenericContainer, Network, Wait } from "testcontainers";

const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, "..", "..");

export const IMAGE_TAG = "lorentz-integration-test:local";

// The binary under test: LORENTZ_BINARY if set, otherwise the most recently built
// of the one ./build.sh leaves in the repository root and the one in the CMake
// build directory
export function findBinary() {
  if (process.env.LORENTZ_BINARY) {
    if (!existsSync(process.env.LORENTZ_BINARY))
      throw new Error(`LORENTZ_BINARY does not exist: ${process.env.LORENTZ_BINARY}`);
    return process.env.LORENTZ_BINARY;
  }
  const candidates = [join(repo, "lorentz"), join(repo, "cmake", "lorentz")].filter((path) => existsSync(path));
  if (candidates.length === 0)
    throw new Error("No lorentz binary found, build it (./build.sh) or set LORENTZ_BINARY");
  candidates.sort((a, b) => statSync(b).mtimeMs - statSync(a).mtimeMs);
  return candidates[0];
}

// Build the runtime image around the binary under test
export async function buildImage() {
  const context = mkdtempSync(join(tmpdir(), "lorentz-integration-"));
  try {
    copyFileSync(join(here, "Dockerfile"), join(context, "Dockerfile"));
    const binary = findBinary();
    console.log(`# lorentz under test: ${binary}`);
    copyFileSync(binary, join(context, "lorentz"));
    copyFileSync(join(repo, "test", "gravity.db.sql"), join(context, "gravity.db.sql"));
    copyFileSync(join(repo, "test", "versions"), join(context, "versions"));
    await GenericContainer.fromDockerfile(context).build(IMAGE_TAG, { deleteOnExit: false });
  } finally {
    rmSync(context, { recursive: true, force: true });
  }
}

// Start Lorentz in a fresh container. The API is open (empty password) unless
// the environment says otherwise. The default upstream list is empty, so
// every answer in these tests comes from Lorentz itself
export async function startLorentz(environment = {}, { command, files = [], network, extraHosts = [] } = {}) {
  let container = new GenericContainer(IMAGE_TAG);
  if (command) container = container.withCommand(command);
  if (files.length) container = container.withCopyFilesToContainer(files);
  if (network) container = container.withNetwork(network);
  if (extraHosts.length) container = container.withExtraHosts(extraHosts);
  return container
    .withEnvironment({
      LORENTZCONF_webserver_api_password: "",
      LORENTZCONF_dns_hosts: "1.2.3.4 local.example.com",
      // Store the network table and the long-term database quickly
      LORENTZCONF_database_DBinterval: "2",
      ...environment,
    })
    .withExposedPorts(80)
    .withWaitStrategy(Wait.forHttp("/api/auth", 80).forStatusCodeMatching((code) => code < 500))
    .withStartupTimeout(120_000)
    .start();
}

// Restart the container. Mapped ports can change, so callers must not cache
// the base URL across this call
export async function restartLorentz(container) {
  // The timeout is in milliseconds. It has to be long enough for Lorentz to export
  // its queries and close the databases, or docker kills it
  await container.restart({ timeout: 30_000 });
  await waitForApi(container);
}

export async function waitForApi(container, timeoutMs = 60_000) {
  const deadline = Date.now() + timeoutMs;
  let last;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${baseUrl(container)}/api/auth`);
      if (response.status < 500) return;
      last = `HTTP ${response.status}`;
    } catch (error) {
      last = error.message;
    }
    await sleep(250);
  }
  throw new Error(`Lorentz API did not come up: ${last}`);
}

export const sleep = (ms) => new Promise((done) => setTimeout(done, ms));

export function baseUrl(container) {
  return `http://${container.getHost()}:${container.getMappedPort(80)}`;
}

// Call the API. Returns { status, body } with the JSON body parsed when there is one
export async function api(container, path, { method = "GET", json, headers = {}, body } = {}) {
  const options = { method, headers: { ...headers } };
  if (json !== undefined) {
    options.headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(json);
  } else if (body !== undefined) {
    options.body = body;
  }
  const response = await fetch(`${baseUrl(container)}${path}`, options);
  const text = await response.text();
  let parsed = text;
  try {
    parsed = text.length ? JSON.parse(text) : null;
  } catch {
    // not JSON (e.g. a ZIP file), the caller asked for it
  }
  return { status: response.status, body: parsed, response };
}

export async function apiBinary(container, path) {
  const response = await fetch(`${baseUrl(container)}${path}`);
  return { status: response.status, data: Buffer.from(await response.arrayBuffer()) };
}

// Run a command in the container, fail loudly if it does not exit cleanly
export async function run(container, command) {
  const result = await container.exec(command);
  if (result.exitCode !== 0) {
    throw new Error(`${command.join(" ")} failed (${result.exitCode}): ${result.output}`);
  }
  return result.output.trim();
}

// Ask Lorentz's own DNS server. Returns the answer lines (empty for no answer)
export async function dig(container, name, type = "A") {
  const result = await container.exec([
    "dig", "+short", "+time=2", "+tries=1", type, name, "@127.0.0.1",
  ]);
  return result.output.split("\n").map((line) => line.trim()).filter(Boolean);
}

// Wait until an async condition holds
export async function eventually(description, check, { timeoutMs = 20_000, intervalMs = 250 } = {}) {
  const deadline = Date.now() + timeoutMs;
  let last;
  while (Date.now() < deadline) {
    try {
      last = await check();
      if (last) return last;
    } catch (error) {
      last = error;
    }
    await sleep(intervalMs);
  }
  throw new Error(`Timed out waiting for: ${description} (last: ${last instanceof Error ? last.message : JSON.stringify(last)})`);
}

// The SQLite shell of the binary under test, so a database is read with the
// same engine that wrote it
export async function sqlite(container, database, sql) {
  return run(container, ["lorentz", "sqlite3", "-batch", database, sql]);
}

export const LORENTZ_DB = "/etc/lorentz/lorentz.db";
export const GRAVITY_DB = "/etc/lorentz/gravity.db";

export async function lorentzLog(container) {
  return run(container, ["cat", "/var/log/lorentz/lorentz.log"]);
}

// Wait until Lorentz has stopped logging for a moment, e.g. after it reloaded its
// lists in the background. Writing to gravity.db from outside while Lorentz is
// reopening it makes Lorentz report "database is locked" (it does not wait for a
// busy database on purpose), which says nothing about Lorentz
export async function settle(container, quietMs = 2500, timeoutMs = 30_000) {
  const lines = async () => (await lorentzLog(container)).split("\n").length;
  const deadline = Date.now() + timeoutMs;
  let count = await lines();
  let since = Date.now();
  while (Date.now() < deadline) {
    await sleep(250);
    const now = await lines();
    if (now !== count) {
      count = now;
      since = Date.now();
    } else if (Date.now() - since >= quietMs) {
      return;
    }
  }
}

// A PostgreSQL server for the long-term database of one or more Lorentz containers,
// on a network that Lorentz can reach it on. Every call of schema() gives a fresh
// schema, so tests do not see each other's data.
export async function startPostgres(image = process.env.PG_IMAGE ?? "postgres:16-alpine") {
  const network = await new Network().start();
  const container = await new GenericContainer(image)
    .withNetwork(network)
    .withNetworkAliases("pg")
    .withEnvironment({ POSTGRES_PASSWORD: "lorentz", POSTGRES_DB: "lorentz_test" })
    .withExposedPorts(5432)
    // The image starts a temporary server for initialization first
    .withWaitStrategy(Wait.forLogMessage(/database system is ready to accept connections/, 2))
    .withStartupTimeout(120_000)
    .start();

  let counter = 0;
  const uri = (schema, host = "pg") =>
    `postgresql://postgres:lorentz@${host}/lorentz_test?options=-c%20search_path%3D${schema}`;
  const psql = async (schema, sql) => {
    const result = await container.exec(["psql", "-At", "-v", "ON_ERROR_STOP=1", uri(schema, "localhost"), "-c", sql]);
    if (result.exitCode !== 0) throw new Error(`psql failed (${result.exitCode}): ${result.output}`);
    return result.output.trim();
  };

  return {
    network,
    container,
    // Create a schema and return what a Lorentz container needs to use it
    async schema() {
      const name = `lz_${Date.now().toString(36)}_${counter++}`;
      await psql("public", `CREATE SCHEMA ${name}`);
      return { name, uri: uri(name), sql: (sql) => psql(name, sql) };
    },
    async stop() {
      await container.stop();
      await network.stop();
    },
  };
}
