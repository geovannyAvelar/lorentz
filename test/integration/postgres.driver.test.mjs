// Runs the PostgreSQL driver regression harness against real PostgreSQL servers
// started with Testcontainers. The harness (test/db_postgres_regression.c) is a
// native binary; it gets the address of the server in POSTGRES_URL.
//
// Build the harness first:
//   cmake -DBUILD_DB_POSTGRES_REGRESSION=ON .. && make db_postgres_regression
// and point LORENTZ_PG_HARNESS at it if it is not in ./cmake.
import { describe, it } from "node:test";
import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { GenericContainer, Wait } from "testcontainers";

const run = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));
const repo = resolve(here, "..", "..");
const harness = process.env.LORENTZ_PG_HARNESS ?? join(repo, "cmake", "db_postgres_regression");

// Server versions to run against. PG_IMAGES="postgres:15-alpine,postgres:16-alpine" overrides
const images = (process.env.PG_IMAGES ?? "postgres:16-alpine,postgres:17-alpine").split(",");

async function startPostgres(image) {
  return new GenericContainer(image)
    .withEnvironment({ POSTGRES_PASSWORD: "lorentz", POSTGRES_DB: "lorentz_test" })
    .withExposedPorts(5432)
    // The image starts a temporary server for initialization first
    .withWaitStrategy(Wait.forLogMessage(/database system is ready to accept connections/, 2))
    .withStartupTimeout(120_000)
    .start();
}

describe("PostgreSQL driver", () => {
  it("has a harness to run", () => {
    assert.ok(existsSync(harness), `build the harness first, not found: ${harness}`);
  });

  for (const image of images) {
    it(`passes its regression harness on ${image}`, async () => {
      const server = await startPostgres(image);
      try {
        const url = `postgresql://postgres:lorentz@${server.getHost()}:${server.getMappedPort(5432)}/lorentz_test`;
        let result;
        try {
          result = await run(harness, [], { env: { ...process.env, POSTGRES_URL: url }, timeout: 300_000 });
        } catch (error) {
          assert.fail(`the harness failed on ${image}:\n${error.stdout ?? ""}\n${error.stderr ?? ""}`);
        }
        assert.match(result.stdout, /DB_POSTGRES_REGRESSION=PASS/);
        assert.match(result.stdout, /\d+ checks, 0 failures/);
      } finally {
        await server.stop();
      }
    });
  }
});
