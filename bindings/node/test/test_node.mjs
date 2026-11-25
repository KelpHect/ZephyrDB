import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { ZephyrPgClient, openEmbedded } from '../index.mjs';

const SERVER_BIN =
  process.env.ZEPHYRDB_SERVER ?? 'd:/Projects/ZephyrDB/build/zephyrdb_server.exe';

function getFreePort() {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const addr = srv.address();
      const port = typeof addr === 'object' && addr ? addr.port : 0;
      srv.close((err) => (err ? reject(err) : resolve(port)));
    });
  });
}

function startServer(binPath, dataDir, port) {
  return new Promise((resolve, reject) => {
    const proc = spawn(binPath, ['--data-dir', dataDir, '--pgwire-port', String(port)], {
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    let settled = false;

    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        proc.kill();
        reject(new Error(`Timed out waiting for ZephyrDB server:\n${output}`));
      }
    }, 10000);

    proc.stdout.on('data', (chunk) => {
      output += chunk.toString('utf8');
      if (!settled && output.includes('ZephyrDB server ready')) {
        settled = true;
        clearTimeout(timer);
        resolve(proc);
      }
    });
    proc.stderr.on('data', (chunk) => {
      output += chunk.toString('utf8');
    });
    proc.on('error', (err) => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        reject(err);
      }
    });
    proc.on('exit', (code) => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        reject(new Error(`ZephyrDB server exited early (${code}):\n${output}`));
      }
    });
  });
}

async function runTests() {
  if (!fs.existsSync(SERVER_BIN)) {
    throw new Error(`Server binary not found at ${SERVER_BIN}`);
  }

  const port = await getFreePort();
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'zephyrdb-node-test-'));
  let proc = null;
  let client = null;

  try {
    proc = await startServer(SERVER_BIN, tmpDir, port);
    client = new ZephyrPgClient({ host: '127.0.0.1', port });
    await client.connect();

    // 1. Benign startup queries (for node-postgres / postgres.js / psql compatibility)
    const ver = await client.query('SELECT version();');
    assert.equal(ver.rows.length, 1);
    assert.match(String(ver.rows[0].version), /ZephyrDB/);

    const dbRes = await client.query('SELECT current_database();');
    assert.equal(dbRes.rows[0].current_database, 'zephyr');

    await client.query("SET client_encoding TO 'UTF8';");
    const showRes = await client.query('SHOW server_version;');
    assert.equal(showRes.rows[0].server_version, '16.0');

    // 2. Simple Query DDL & INSERT
    await client.query(
      'CREATE TABLE scores (id BIGINT PRIMARY KEY, player TEXT, score BIGINT, active BOOLEAN);'
    );
    await client.query("INSERT INTO scores VALUES (1, 'Alice', 100, true);");

    // 3. Extended Query ($1/$2/$3/$4) + INSERT ... RETURNING
    const ins1 = await client.query(
      'INSERT INTO scores (id, player, score, active) VALUES ($1, $2, $3, $4) RETURNING id, player, score, active;',
      [2, 'Bob', 250, true]
    );
    assert.equal(ins1.rows.length, 1);
    assert.equal(ins1.rows[0].id, 2);
    assert.equal(ins1.rows[0].player, 'Bob');
    assert.equal(ins1.rows[0].score, 250);
    assert.equal(ins1.rows[0].active, true);

    const ins2 = await client.query(
      'INSERT INTO scores (id, player, score, active) VALUES ($1, $2, $3, $4) RETURNING id, player, score, active;',
      [3, 'Carol', 175, false]
    );
    assert.equal(ins2.rows.length, 1);
    assert.equal(ins2.rows[0].player, 'Carol');
    assert.equal(ins2.rows[0].active, false);

    // 4. Extended Query SELECT with $1 parameter
    const filtered = await client.query(
      'SELECT id, player, score FROM scores WHERE score >= $1 ORDER BY score DESC;',
      [150]
    );
    assert.equal(filtered.rows.length, 2);
    assert.equal(filtered.rows[0].player, 'Bob');
    assert.equal(filtered.rows[0].score, 250);
    assert.equal(filtered.rows[1].player, 'Carol');
    assert.equal(filtered.rows[1].score, 175);

    // 5. WITH CTE query
    const cte = await client.query(
      'WITH top_players AS (SELECT id, player, score FROM scores WHERE score >= $1) SELECT player, score FROM top_players ORDER BY score DESC;',
      [100]
    );
    assert.equal(cte.rows.length, 3);
    assert.deepEqual(
      cte.rows.map((r) => r.player),
      ['Bob', 'Carol', 'Alice']
    );

    // 6. O(1) COUNT(*) + SUM(score)
    const agg = await client.query('SELECT COUNT(*), SUM(score) FROM scores;');
    assert.equal(agg.rows.length, 1);
    const aggVals = Object.values(agg.rows[0]);
    assert.equal(aggVals[0], 3);
    assert.equal(aggVals[1], 525);

    // 7. openEmbedded() graceful error when N-API binary is not compiled
    const napiRelease = path.resolve('build/Release/zephyr_napi.node');
    if (!fs.existsSync(napiRelease)) {
      assert.throws(() => openEmbedded({ dataDir: tmpDir }), /not built/i);
    }

    console.log('All Node.js PGWire v3.0 tests passed!');
  } finally {
    if (client) {
      await client.close().catch(() => {});
    }
    if (proc && !proc.killed) {
      proc.kill();
    }
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

runTests().catch((err) => {
  console.error(err);
  process.exit(1);
});
