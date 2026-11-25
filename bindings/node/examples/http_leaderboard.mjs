import http from 'node:http';
import { ZephyrPgClient } from '../index.mjs';

const PG_HOST = process.env.ZEPHYR_PG_HOST ?? '127.0.0.1';
const PG_PORT = Number(process.env.ZEPHYR_PG_PORT ?? 5432);
const HTTP_PORT = Number(process.env.PORT ?? 3000);

const client = new ZephyrPgClient({ host: PG_HOST, port: PG_PORT });

async function initSchema() {
  try {
    await client.query(
      'CREATE TABLE scores (id BIGINT PRIMARY KEY, player TEXT, score BIGINT);'
    );
  } catch {
    // Table may already exist across restarts
  }
}

function readJsonBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      try {
        const raw = Buffer.concat(chunks).toString('utf8');
        resolve(raw ? JSON.parse(raw) : {});
      } catch (err) {
        reject(err);
      }
    });
    req.on('error', reject);
  });
}

function sendJson(res, status, data) {
  const payload = JSON.stringify(data);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
  });
  res.end(payload);
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url ?? '/', `http://${req.headers.host ?? 'localhost'}`);

  try {
    if (req.method === 'GET' && url.pathname === '/leaderboard') {
      const result = await client.query(
        'SELECT id, player, score FROM scores ORDER BY score DESC LIMIT 20;'
      );
      return sendJson(res, 200, { leaderboard: result.rows });
    }

    if (req.method === 'GET' && url.pathname === '/stats') {
      // O(1) Augmented B-Tree COUNT(*) and SUM(score)
      const result = await client.query('SELECT COUNT(*), SUM(score) FROM scores;');
      const row = result.rows[0] ?? {};
      const values = Object.values(row);
      return sendJson(res, 200, {
        totalPlayers: Number(values[0] ?? 0),
        totalScore: Number(values[1] ?? 0),
      });
    }

    if (req.method === 'POST' && url.pathname === '/score') {
      const body = await readJsonBody(req);
      const id = Number(body.id ?? Date.now());
      const player = String(body.player ?? '');
      const score = Number(body.score ?? 0);

      if (!player || !Number.isFinite(score) || !Number.isFinite(id)) {
        return sendJson(res, 400, { error: 'Expected { id?, player, score }' });
      }

      const result = await client.query(
        'INSERT INTO scores (id, player, score) VALUES ($1, $2, $3) RETURNING id, player, score;',
        [id, player, score]
      );
      return sendJson(res, 201, { inserted: result.rows[0] ?? null });
    }

    sendJson(res, 404, { error: 'Not found' });
  } catch (err) {
    sendJson(res, 500, { error: err instanceof Error ? err.message : String(err) });
  }
});

async function main() {
  await client.connect();
  await initSchema();
  server.listen(HTTP_PORT, '127.0.0.1', () => {
    console.log(`Leaderboard API listening on http://127.0.0.1:${HTTP_PORT}`);
    console.log(`Connected to ZephyrDB PGWire at ${PG_HOST}:${PG_PORT}`);
  });
}

process.on('SIGINT', async () => {
  server.close();
  await client.close();
  process.exit(0);
});

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
