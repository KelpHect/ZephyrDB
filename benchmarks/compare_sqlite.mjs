import { DatabaseSync } from 'node:sqlite';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { ZephyrPgClient } from '../bindings/node/index.mjs';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(__dirname, '..');

function formatUs(us) {
  if (us === null || us === undefined || Number.isNaN(us)) return 'N/A';
  if (us >= 1000) return `${(us / 1000).toFixed(2)} ms`;
  return `${us.toFixed(2)} µs`;
}

function measureSqlite(modeLabel, dbPath) {
  const db = new DatabaseSync(dbPath);
  if (dbPath !== ':memory:') {
    db.exec('PRAGMA journal_mode = WAL;');
    db.exec('PRAGMA synchronous = NORMAL;');
  }

  db.exec('CREATE TABLE items (id INTEGER PRIMARY KEY, gold INTEGER);');
  db.exec('CREATE TABLE items2 (id INTEGER PRIMARY KEY, gold INTEGER);');

  // Seed 10k rows in items and 1k rows in items2 inside a transaction
  db.exec('BEGIN;');
  const seed1 = db.prepare('INSERT INTO items (id, gold) VALUES (?, ?);');
  for (let i = 1; i <= 10000; i++) {
    seed1.run(i, i * 10);
  }
  const seed2 = db.prepare('INSERT INTO items2 (id, gold) VALUES (?, ?);');
  for (let i = 1; i <= 1000; i++) {
    seed2.run(i, i * 5);
  }
  db.exec('COMMIT;');

  const pkSel = db.prepare('SELECT id, gold FROM items WHERE id = ?;');
  const aggSel = db.prepare('SELECT COUNT(*) AS cnt, SUM(gold) AS total FROM items;');
  const pkIns = db.prepare('INSERT INTO items (id, gold) VALUES (?, ?);');
  const joinSel = db.prepare(
    'SELECT a.id, a.gold FROM items a INNER JOIN items2 b ON a.id = b.id ORDER BY a.gold DESC LIMIT 10;'
  );

  // Warmup
  for (let i = 0; i < 100; i++) pkSel.get(5000);
  aggSel.get();
  joinSel.all();

  // 1 PK SELECT (averaged over 5000 runs)
  const pkIters = 5000;
  let t0 = performance.now();
  for (let i = 0; i < pkIters; i++) {
    pkSel.get(5000);
  }
  const onePkUs = ((performance.now() - t0) * 1000) / pkIters;

  // 50 PK SELECTs (actual 50-query batch, averaged over 100 batches)
  const batchIters = 100;
  t0 = performance.now();
  for (let b = 0; b < batchIters; b++) {
    for (let i = 1; i <= 50; i++) {
      pkSel.get(i * 100);
    }
  }
  const fiftyPkUs = ((performance.now() - t0) * 1000) / batchIters;

  // SELECT COUNT(*), SUM(gold) FROM items (averaged over 200 runs)
  const aggIters = 200;
  t0 = performance.now();
  for (let i = 0; i < aggIters; i++) {
    aggSel.get();
  }
  const aggUs = ((performance.now() - t0) * 1000) / aggIters;

  // 1 INSERT (prepared)
  t0 = performance.now();
  pkIns.run(999999, 42);
  const oneInsUs = (performance.now() - t0) * 1000;

  // 50 INSERTs (prepared)
  t0 = performance.now();
  for (let i = 0; i < 50; i++) {
    pkIns.run(1000000 + i, i * 10);
  }
  const fiftyInsUs = (performance.now() - t0) * 1000;

  // JOIN + ORDER BY LIMIT 10 (averaged over 50 runs)
  const joinIters = 50;
  t0 = performance.now();
  for (let i = 0; i < joinIters; i++) {
    joinSel.all();
  }
  const joinUs = ((performance.now() - t0) * 1000) / joinIters;

  db.close();
  return {
    label: modeLabel,
    onePkUs,
    fiftyPkUs,
    aggUs,
    oneInsUs,
    fiftyInsUs,
    joinUs,
  };
}

function runZephyrBenchExe() {
  const candidates = [
    path.join(REPO_ROOT, 'build', 'zephyrdb_bench.exe'),
    path.join(REPO_ROOT, 'build', 'zephyrdb_bench'),
  ];
  const exePath = candidates.find((p) => fs.existsSync(p));
  if (!exePath) return null;

  try {
    const out = execFileSync(exePath, [], {
      cwd: REPO_ROOT,
      encoding: 'utf8',
      timeout: 30000,
    });
    const pick = (re) => {
      const m = out.match(re);
      return m ? Number(m[1]) : null;
    };
    return {
      label: 'ZephyrDB (C-ABI)',
      onePkUs: pick(/Everyday SQL \(1 PK SELECT, 10k rows\):\s*([\d.]+)\s*us/),
      fiftyPkUs: pick(/Everyday SQL \(50 PK SELECTs, 10k rows\):\s*([\d.]+)\s*us/),
      aggUs: pick(/SQL Prepared O\(1\) COUNT\+SUM roundtrip:\s*([\d.]+)\s*us/),
      oneInsUs: pick(/Everyday SQL \(1 INSERT\):\s*([\d.]+)\s*us/),
      fiftyInsUs: pick(/Everyday SQL \(50 INSERTs\):\s*([\d.]+)\s*us/),
      joinUs: pick(/Everyday SQL \(JOIN \+ ORDER BY LIMIT 10\):\s*([\d.]+)\s*us/),
    };
  } catch {
    return null;
  }
}

async function tryZephyrPgWire() {
  const host = process.env.ZEPHYR_PG_HOST ?? '127.0.0.1';
  const port = Number(process.env.ZEPHYR_PG_PORT ?? 5432);
  const client = new ZephyrPgClient({ host, port });

  try {
    await Promise.race([
      client.connect(),
      new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), 600)),
    ]);
  } catch {
    return null;
  }

  try {
    await client.query('DROP TABLE IF EXISTS bench_items;');
    await client.query('DROP TABLE IF EXISTS bench_items2;');
    await client.query('CREATE TABLE bench_items (id BIGINT PRIMARY KEY, gold BIGINT);');
    await client.query('CREATE TABLE bench_items2 (id BIGINT PRIMARY KEY, gold BIGINT);');

    // Seed 10k + 1k rows
    for (let i = 1; i <= 10000; i += 250) {
      const batch = [];
      for (let j = i; j < i + 250 && j <= 10000; j++) {
        batch.push({ sql: 'INSERT INTO bench_items VALUES ($1, $2);', params: [j, j * 10] });
      }
      if (typeof client.pipeline === 'function') {
        await client.pipeline(batch);
      } else {
        for (const q of batch) await client.query(q.sql, q.params);
      }
    }
    for (let i = 1; i <= 1000; i += 250) {
      const batch = [];
      for (let j = i; j < i + 250 && j <= 1000; j++) {
        batch.push({ sql: 'INSERT INTO bench_items2 VALUES ($1, $2);', params: [j, j * 5] });
      }
      if (typeof client.pipeline === 'function') {
        await client.pipeline(batch);
      } else {
        for (const q of batch) await client.query(q.sql, q.params);
      }
    }

    // 1 PK SELECT
    const pkIters = 100;
    let t0 = performance.now();
    for (let i = 0; i < pkIters; i++) {
      await client.query('SELECT id, gold FROM bench_items WHERE id = $1;', [5000]);
    }
    const onePkUs = ((performance.now() - t0) * 1000) / pkIters;

    // 50 PK SELECTs (pipelined in 1 RTT if supported, else sequential)
    const queries50 = Array.from({ length: 50 }, (_, idx) => ({
      sql: 'SELECT id, gold FROM bench_items WHERE id = $1;',
      params: [(idx + 1) * 100],
    }));
    const batchIters = 20;
    t0 = performance.now();
    for (let b = 0; b < batchIters; b++) {
      if (typeof client.pipeline === 'function') {
        await client.pipeline(queries50);
      } else {
        for (const q of queries50) await client.query(q.sql, q.params);
      }
    }
    const fiftyPkUs = ((performance.now() - t0) * 1000) / batchIters;

    // COUNT + SUM
    const aggIters = 50;
    t0 = performance.now();
    for (let i = 0; i < aggIters; i++) {
      await client.query('SELECT COUNT(*), SUM(gold) FROM bench_items;');
    }
    const aggUs = ((performance.now() - t0) * 1000) / aggIters;

    // 1 INSERT
    t0 = performance.now();
    await client.query('INSERT INTO bench_items VALUES ($1, $2);', [999999, 42]);
    const oneInsUs = (performance.now() - t0) * 1000;

    // 50 INSERTs
    const ins50 = Array.from({ length: 50 }, (_, idx) => ({
      sql: 'INSERT INTO bench_items VALUES ($1, $2);',
      params: [1000000 + idx, idx * 10],
    }));
    t0 = performance.now();
    if (typeof client.pipeline === 'function') {
      await client.pipeline(ins50);
    } else {
      for (const q of ins50) await client.query(q.sql, q.params);
    }
    const fiftyInsUs = (performance.now() - t0) * 1000;

    // JOIN + ORDER BY LIMIT 10
    const joinIters = 10;
    t0 = performance.now();
    for (let i = 0; i < joinIters; i++) {
      await client.query(
        'SELECT a.id, a.gold FROM bench_items a INNER JOIN bench_items2 b ON a.id = b.id ORDER BY a.gold DESC LIMIT 10;'
      );
    }
    const joinUs = ((performance.now() - t0) * 1000) / joinIters;

    await client.query('DROP TABLE IF EXISTS bench_items;');
    await client.query('DROP TABLE IF EXISTS bench_items2;');

    return {
      label: typeof client.pipeline === 'function' ? 'ZephyrDB (PGWire+Pipe)' : 'ZephyrDB (PGWire)',
      onePkUs,
      fiftyPkUs,
      aggUs,
      oneInsUs,
      fiftyInsUs,
      joinUs,
    };
  } finally {
    await client.close();
  }
}

function printComparisonTable(results) {
  const metrics = [
    { key: 'onePkUs', name: '1 PK SELECT (10k rows)' },
    { key: 'fiftyPkUs', name: '50 PK SELECTs (10k rows)' },
    { key: 'aggUs', name: 'SELECT COUNT(*), SUM(gold)' },
    { key: 'oneInsUs', name: '1 INSERT (prepared)' },
    { key: 'fiftyInsUs', name: '50 INSERTs (prepared)' },
    { key: 'joinUs', name: 'JOIN + ORDER BY LIMIT 10' },
  ];

  const colWidths = [
    Math.max(28, ...metrics.map((m) => m.name.length)) + 2,
    ...results.map((r) => Math.max(r.label.length, 14) + 2),
  ];

  const pad = (s, w) => String(s).padEnd(w);
  const padR = (s, w) => String(s).padStart(w - 2) + '  ';

  const header =
    pad('Query (99% Everyday SQL)', colWidths[0]) +
    results.map((r, i) => padR(r.label, colWidths[i + 1])).join('');
  const sep = '-'.repeat(header.length);

  console.log('');
  console.log(`=== SQLite (Node ${process.version}) vs ZephyrDB Everyday SQL ===`);
  console.log(sep);
  console.log(header);
  console.log(sep);
  for (const m of metrics) {
    const row =
      pad(m.name, colWidths[0]) +
      results.map((r, i) => padR(formatUs(r[m.key]), colWidths[i + 1])).join('');
    console.log(row);
  }
  console.log(sep);
  console.log('');
}

async function main() {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'zephyr-sqlite-bench-'));
  const walDbPath = path.join(tmpDir, 'bench_wal.sqlite');

  try {
    const results = [];
    results.push(measureSqlite('SQLite (:memory:)', ':memory:'));
    results.push(measureSqlite('SQLite (WAL)', walDbPath));

    const zephyrExe = runZephyrBenchExe();
    if (zephyrExe) results.push(zephyrExe);

    const zephyrPg = await tryZephyrPgWire();
    if (zephyrPg) results.push(zephyrPg);

    printComparisonTable(results);
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

await main();
