export interface ZephyrPgClientOptions {
  host?: string;
  port?: number;
  user?: string;
  database?: string;
}

export interface QueryField {
  name: string;
  dataTypeID: number;
}

export interface QueryResult<T = Record<string, unknown>> {
  rows: T[];
  rowCount: number;
  command: string;
  fields: QueryField[];
}

export type PipelineQueryItem = string | { sql: string; params?: unknown[] };

export class ZephyrPgClient {
  constructor(options?: ZephyrPgClientOptions);
  connect(): Promise<void>;
  query<T = Record<string, unknown>>(sql: string, params?: unknown[]): Promise<QueryResult<T>>;
  pipeline<T = Record<string, unknown>>(queries: PipelineQueryItem[]): Promise<Array<QueryResult<T>>>;
  close(): Promise<void>;
}

export interface ZephyrEmbeddedConfig {
  dataDir?: string;
  enableWal?: boolean;
  enableCompactor?: boolean;
}

export interface EmbeddedResultSet<T = Record<string, unknown>> {
  rowCount: number;
  colCount: number;
  rowsAffected: number;
  columns: Array<{ name: string; type: number }>;
  rows: T[];
}

export interface EmbeddedPreparedStmt {
  query<T = Record<string, unknown>>(params?: unknown[]): EmbeddedResultSet<T>;
  exec(params?: unknown[]): number;
}

export interface EmbeddedAggregateSum {
  count: number;
  sumI64: number;
  sumF64: number;
}

export interface ZephyrStats {
  tableCount: number;
  totalRows: number;
  walLsn: number;
  walFlushedLsn: number;
  rcuReclaimed: number;
  o1FastpathHits: number;
  preparedQueries: number;
  activeTxns: number;
}

export interface EmbeddedDB {
  exec(sql: string): number;
  execSync(sql: string): number;
  query<T = Record<string, unknown>>(sql: string, params?: unknown[]): EmbeddedResultSet<T>;
  prepare(sql: string): EmbeddedPreparedStmt;
  aggregateCount(table: string): number;
  aggregateSum(table: string, col: string): EmbeddedAggregateSum;
  checkpoint(): boolean;
  getStats(): ZephyrStats;
  close(): void;
}

export function openEmbedded(config?: ZephyrEmbeddedConfig | string): EmbeddedDB;
export function openEncrypted(config: ZephyrEmbeddedConfig | string, keyBuffer: Buffer): EmbeddedDB;
