import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);

function buildMessage(type, bodyBuf) {
  const buf = Buffer.allocUnsafe(5 + bodyBuf.length);
  buf.writeUInt8(type.charCodeAt(0), 0);
  buf.writeInt32BE(4 + bodyBuf.length, 1);
  bodyBuf.copy(buf, 5);
  return buf;
}

function buildStartupMessage(user, database) {
  const parts = [
    Buffer.from('user\0', 'utf8'),
    Buffer.from(`${user}\0`, 'utf8'),
    Buffer.from('database\0', 'utf8'),
    Buffer.from(`${database}\0`, 'utf8'),
    Buffer.from([0]),
  ];
  const body = Buffer.concat(parts);
  const out = Buffer.allocUnsafe(8 + body.length);
  out.writeInt32BE(8 + body.length, 0);
  out.writeInt32BE(196608, 4); // PGWire v3.0
  body.copy(out, 8);
  return out;
}

function decodePgValue(str, oid) {
  switch (oid) {
    case 20: // INT8
    case 21: // INT2
    case 23: // INT4
      return Number(str);
    case 700: // FLOAT4
    case 701: // FLOAT8
      return Number(str);
    case 16: // BOOL
      return str === 't' || str === 'true' || str === '1';
    case 17: // BYTEA
      if (str.startsWith('\\x')) {
        return Buffer.from(str.slice(2), 'hex');
      }
      return Buffer.from(str, 'utf8');
    default:
      return str;
  }
}

function parseErrorBody(body) {
  let pos = 0;
  let message = 'PostgreSQL error';
  while (pos < body.length) {
    const code = body[pos++];
    if (code === 0) break;
    const nul = body.indexOf(0, pos);
    if (nul === -1) break;
    const val = body.toString('utf8', pos, nul);
    pos = nul + 1;
    if (code === 77 /* 'M' */) {
      message = val;
    }
  }
  return message;
}

function parseCommandRowCount(tag, rows) {
  if (rows.length > 0) return rows.length;
  const parts = tag.trim().split(/\s+/);
  const last = parts[parts.length - 1];
  const n = Number(last);
  return Number.isFinite(n) ? n : 0;
}

function parseRowDescription(body) {
  const fields = [];
  const ncols = body.readInt16BE(0);
  let pos = 2;
  for (let i = 0; i < ncols; i++) {
    const nul = body.indexOf(0, pos);
    const name = body.toString('utf8', pos, nul);
    pos = nul + 1;
    pos += 6; // tableOid (4) + attrNum (2)
    const dataTypeID = body.readInt32BE(pos);
    pos += 4;
    pos += 8; // typlen (2) + typmod (4) + fmt (2)
    fields.push({ name, dataTypeID });
  }
  return fields;
}

function parseDataRow(body, fields) {
  const ncols = body.readInt16BE(0);
  let pos = 2;
  const row = {};
  for (let i = 0; i < ncols; i++) {
    const vlen = body.readInt32BE(pos);
    pos += 4;
    const field = fields[i];
    if (vlen === -1) {
      row[field.name] = null;
    } else {
      const raw = body.toString('utf8', pos, pos + vlen);
      pos += vlen;
      row[field.name] = decodePgValue(raw, field.dataTypeID);
    }
  }
  return row;
}

function buildExtendedFrames(sql, params = [], stmtName = '', portalName = '') {
  const stmtBuf = Buffer.from(`${stmtName}\0`, 'utf8');
  const sqlBuf = Buffer.from(`${sql}\0`, 'utf8');
  const parseBody = Buffer.allocUnsafe(stmtBuf.length + sqlBuf.length + 2);
  stmtBuf.copy(parseBody, 0);
  sqlBuf.copy(parseBody, stmtBuf.length);
  parseBody.writeInt16BE(0, stmtBuf.length + sqlBuf.length); // 0 param OIDs specified

  const portalBuf = Buffer.from(`${portalName}\0`, 'utf8');
  const fmtHdr = Buffer.allocUnsafe(4);
  fmtHdr.writeInt16BE(0, 0); // 0 param format codes (all text)
  fmtHdr.writeInt16BE(params.length, 2);
  const bindParts = [portalBuf, stmtBuf, fmtHdr];

  for (const p of params) {
    if (p === null || p === undefined) {
      const lenBuf = Buffer.allocUnsafe(4);
      lenBuf.writeInt32BE(-1, 0);
      bindParts.push(lenBuf);
    } else {
      const str = typeof p === 'boolean' ? (p ? 'true' : 'false') : String(p);
      const valBuf = Buffer.from(str, 'utf8');
      const lenBuf = Buffer.allocUnsafe(4);
      lenBuf.writeInt32BE(valBuf.length, 0);
      bindParts.push(lenBuf, valBuf);
    }
  }
  const resFmt = Buffer.allocUnsafe(2);
  resFmt.writeInt16BE(0, 0); // all text results
  bindParts.push(resFmt);
  const bindBody = Buffer.concat(bindParts);

  const execBody = Buffer.allocUnsafe(portalBuf.length + 4);
  portalBuf.copy(execBody, 0);
  execBody.writeInt32BE(0, portalBuf.length); // max_rows = 0 (unlimited)

  return [
    buildMessage('P', parseBody),
    buildMessage('B', bindBody),
    buildMessage('E', execBody),
  ];
}

export class ZephyrPgClient {
  constructor(options = {}) {
    this.host = options.host ?? '127.0.0.1';
    this.port = options.port ?? 5432;
    this.user = options.user ?? 'zephyr';
    this.database = options.database ?? 'zephyr';
    this.socket = null;
    this.rxBuf = Buffer.alloc(0);
    this.waiter = null;
    this.closed = false;
    this.serverParams = {};
    this.queue = Promise.resolve();
  }

  async connect() {
    if (this.socket) return;
    await new Promise((resolve, reject) => {
      const sock = net.createConnection({ host: this.host, port: this.port }, () => {
        sock.setNoDelay(true);
        resolve();
      });
      sock.on('error', reject);
      this.socket = sock;
    });

    this.socket.on('data', (chunk) => {
      this.rxBuf = this.rxBuf.length === 0 ? chunk : Buffer.concat([this.rxBuf, chunk]);
      this._drain();
    });
    this.socket.on('close', () => {
      this.closed = true;
      if (this.waiter) {
        const { reject } = this.waiter;
        this.waiter = null;
        reject(new Error('PGWire connection closed'));
      }
    });
    this.socket.on('error', (err) => {
      if (this.waiter) {
        const { reject } = this.waiter;
        this.waiter = null;
        reject(err);
      }
    });

    this.socket.write(buildStartupMessage(this.user, this.database));
    await this._awaitUntilReady();
  }

  _drain() {
    while (this.waiter && this.rxBuf.length >= 5) {
      const type = String.fromCharCode(this.rxBuf[0]);
      const len = this.rxBuf.readInt32BE(1);
      if (len < 4 || this.rxBuf.length < 1 + len) break;
      const body = this.rxBuf.subarray(5, 1 + len);
      this.rxBuf = this.rxBuf.subarray(1 + len);
      this.waiter.onMessage(type, body);
    }
  }

  _awaitUntilReady() {
    return new Promise((resolve, reject) => {
      let fields = [];
      let rows = [];
      let command = '';
      let errMsg = null;

      this.waiter = {
        reject,
        onMessage: (type, body) => {
          if (type === 'R') {
            // AuthenticationOk (0)
          } else if (type === 'S') {
            const z1 = body.indexOf(0, 0);
            const z2 = body.indexOf(0, z1 + 1);
            if (z1 !== -1 && z2 !== -1) {
              const k = body.toString('utf8', 0, z1);
              const v = body.toString('utf8', z1 + 1, z2);
              this.serverParams[k] = v;
            }
          } else if (type === 'K') {
            // BackendKeyData
          } else if (type === '1' || type === '2' || type === '3' || type === 'n') {
            // ParseComplete / BindComplete / CloseComplete / NoData
          } else if (type === 'T') {
            fields = parseRowDescription(body);
          } else if (type === 'D') {
            rows.push(parseDataRow(body, fields));
          } else if (type === 'C') {
            const nul = body.indexOf(0);
            command = body.toString('utf8', 0, nul === -1 ? body.length : nul);
          } else if (type === 'I') {
            command = 'EMPTY';
          } else if (type === 'E') {
            errMsg = parseErrorBody(body);
          } else if (type === 'Z') {
            this.waiter = null;
            if (errMsg !== null) {
              reject(new Error(errMsg));
            } else {
              resolve({
                rows,
                rowCount: parseCommandRowCount(command, rows),
                command,
                fields,
              });
            }
          }
        },
      };
      this._drain();
    });
  }

  _awaitPipelineReady() {
    return new Promise((resolve, reject) => {
      const results = [];
      let fields = [];
      let rows = [];
      let errMsg = null;

      this.waiter = {
        reject,
        onMessage: (type, body) => {
          if (type === 'R' || type === 'K' || type === '1' || type === '2' || type === '3' || type === 'n') {
            // AuthenticationOk / BackendKeyData / ParseComplete / BindComplete / CloseComplete / NoData
          } else if (type === 'S') {
            const z1 = body.indexOf(0, 0);
            const z2 = body.indexOf(0, z1 + 1);
            if (z1 !== -1 && z2 !== -1) {
              this.serverParams[body.toString('utf8', 0, z1)] = body.toString('utf8', z1 + 1, z2);
            }
          } else if (type === 'T') {
            fields = parseRowDescription(body);
          } else if (type === 'D') {
            rows.push(parseDataRow(body, fields));
          } else if (type === 'C') {
            const nul = body.indexOf(0);
            const command = body.toString('utf8', 0, nul === -1 ? body.length : nul);
            results.push({
              rows,
              rowCount: parseCommandRowCount(command, rows),
              command,
              fields,
            });
            fields = [];
            rows = [];
          } else if (type === 'I') {
            results.push({
              rows: [],
              rowCount: 0,
              command: 'EMPTY',
              fields: [],
            });
            fields = [];
            rows = [];
          } else if (type === 'E') {
            if (errMsg === null) {
              errMsg = parseErrorBody(body);
            }
          } else if (type === 'Z') {
            this.waiter = null;
            if (errMsg !== null) {
              reject(new Error(errMsg));
            } else {
              resolve(results);
            }
          }
        },
      };
      this._drain();
    });
  }

  query(sql, params) {
    const run = async () => {
      if (!this.socket || this.closed) {
        throw new Error('ZephyrPgClient is not connected');
      }
      if (!Array.isArray(params) || params.length === 0) {
        const body = Buffer.from(`${sql}\0`, 'utf8');
        this.socket.write(buildMessage('Q', body));
        return this._awaitUntilReady();
      }

      // Extended Query: Parse ('P') + Bind ('B') + Execute ('E') + Sync ('S')
      const packet = Buffer.concat([
        ...buildExtendedFrames(sql, params),
        buildMessage('S', Buffer.alloc(0)),
      ]);
      this.socket.write(packet);
      return this._awaitUntilReady();
    };

    const next = this.queue.then(run, run);
    this.queue = next.catch(() => {});
    return next;
  }

  async pipeline(queries) {
    const run = async () => {
      if (!this.socket || this.closed) {
        throw new Error('ZephyrPgClient is not connected');
      }
      if (!Array.isArray(queries)) {
        throw new TypeError('queries must be an Array');
      }
      if (queries.length === 0) {
        return [];
      }

      const frames = [];
      for (let i = 0; i < queries.length; i++) {
        const item = queries[i];
        const sql = typeof item === 'string' ? item : item.sql;
        const params = typeof item === 'string' ? [] : (Array.isArray(item.params) ? item.params : []);
        frames.push(...buildExtendedFrames(sql, params, `s${i}`, `p${i}`));
      }
      frames.push(buildMessage('S', Buffer.alloc(0)));

      this.socket.write(Buffer.concat(frames));
      return this._awaitPipelineReady();
    };

    const next = this.queue.then(run, run);
    this.queue = next.catch(() => {});
    return next;
  }

  async close() {
    if (!this.socket || this.closed) return;
    this.closed = true;
    try {
      this.socket.write(buildMessage('X', Buffer.alloc(0)));
    } catch {
      // ignore
    }
    await new Promise((resolve) => {
      this.socket.end(resolve);
    });
    this.socket = null;
  }
}

function loadNativeAddon() {
  const candidates = [
    path.join(__dirname, 'build', 'Release', 'zephyr_napi.node'),
    path.join(__dirname, 'build', 'Debug', 'zephyr_napi.node'),
  ];
  const found = candidates.find((p) => fs.existsSync(p));
  if (!found) {
    throw new Error(
      'ZephyrDB native N-API addon (zephyr_napi.node) is not built. ' +
      'Run `npm run build:napi` (or `node-gyp rebuild`) inside bindings/node/ to compile it.'
    );
  }
  return require(found);
}

function wrapNativeDb(native, db) {
  return {
    exec: (sql) => native.exec(db, sql),
    execSync: (sql) => native.execSync(db, sql),
    query: (sql, params) => native.query(db, sql, params),
    prepare: (sql) => {
      const stmt = native.prepare(db, sql);
      return {
        query: (params) => native.stmtQuery(stmt, params),
        exec: (params) => native.stmtExec(stmt, params),
      };
    },
    aggregateCount: (table) => native.aggregateCount(db, table),
    aggregateSum: (table, col) => native.aggregateSum(db, table, col),
    checkpoint: () => native.checkpoint(db),
    getStats: () => native.getStats(db),
    close: () => native.close(db),
  };
}

export function openEmbedded(config = {}) {
  const native = loadNativeAddon();
  const db = native.open(config);
  return wrapNativeDb(native, db);
}

export function openEncrypted(config = {}, keyBuffer) {
  const native = loadNativeAddon();
  const db = native.openEncrypted(config, keyBuffer);
  return wrapNativeDb(native, db);
}
