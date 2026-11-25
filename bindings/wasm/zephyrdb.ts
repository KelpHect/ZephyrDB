export enum ZephyrStatus {
  Ok = 0,
  ErrInvalidArg = 1,
  ErrSyntax = 2,
  ErrNotFound = 3,
  ErrConstraint = 4,
  ErrIo = 5,
  ErrInternal = 6,
  ErrBusy = 7,
}

export enum ZephyrType {
  Null = 0,
  I64 = 1,
  F64 = 2,
  Bool = 3,
  Text = 4,
  Bytea = 5,
}

export interface ZephyrWasmModule {
  HEAPU8: Uint8Array;
  HEAP32: Int32Array;
  HEAPU32: Uint32Array;
  HEAPF64: Float64Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
  UTF8ToString(ptr: number, maxBytesToRead?: number): string;
  stringToUTF8(str: string, outPtr: number, maxBytesToWrite: number): void;
  lengthBytesUTF8(str: string): number;

  _zephyr_db_open(configPtr: number): number;
  _zephyr_db_close(dbPtr: number): void;
  _zephyr_exec(dbPtr: number, sqlPtr: number, rowsAffectedPtr: number): number;
  _zephyr_query(dbPtr: number, sqlPtr: number): number;
  _zephyr_result_free(resultPtr: number): void;
  _zephyr_last_error(dbPtr: number): number;
  _zephyr_checkpoint(dbPtr: number): number;
}

// wasm32 C-ABI offsets (matching include/zephyr/c_abi.h)
const RS_ROW_COUNT_OFF = 0; // u64
const RS_COL_COUNT_OFF = 8; // u64
const RS_ROWS_AFFECTED_OFF = 16; // u64
const RS_COLUMNS_PTR_OFF = 24; // u32
const RS_VALUES_PTR_OFF = 28; // u32
const RS_STATUS_OFF = 36; // u32
const RS_FLAGS_OFF = 40; // u32

const COL_DESC_SIZE = 12;
const COL_NAME_PTR_OFF = 0;
const COL_NAME_LEN_OFF = 4;
const COL_TYPE_OFF = 8;

const VAL_SIZE = 24;
const VAL_TYPE_OFF = 0;
const VAL_UNION_OFF = 8;

const CFG_SIZE = 32;
const textDecoder = new TextDecoder("utf-8");

export class ZephyrWasmResult {
  private ptr: number;
  private readonly mod: ZephyrWasmModule;

  constructor(mod: ZephyrWasmModule, ptr: number) {
    this.mod = mod;
    this.ptr = ptr;
  }

  private view(): DataView {
    return new DataView(this.mod.HEAPU8.buffer);
  }

  get rowCount(): number {
    if (!this.ptr) return 0;
    return Number(this.view().getBigUint64(this.ptr + RS_ROW_COUNT_OFF, true));
  }

  get colCount(): number {
    if (!this.ptr) return 0;
    return Number(this.view().getBigUint64(this.ptr + RS_COL_COUNT_OFF, true));
  }

  get rowsAffected(): number {
    if (!this.ptr) return 0;
    return Number(this.view().getBigUint64(this.ptr + RS_ROWS_AFFECTED_OFF, true));
  }

  get status(): ZephyrStatus {
    if (!this.ptr) return ZephyrStatus.ErrInternal;
    return this.mod.HEAPU32[(this.ptr + RS_STATUS_OFF) >> 2] as ZephyrStatus;
  }

  get isO1FastPath(): boolean {
    if (!this.ptr) return false;
    return (this.mod.HEAPU32[(this.ptr + RS_FLAGS_OFF) >> 2] & 1) !== 0;
  }

  getColumnName(col: number): string {
    const colsPtr = this.mod.HEAPU32[(this.ptr + RS_COLUMNS_PTR_OFF) >> 2];
    const base = colsPtr + col * COL_DESC_SIZE;
    const namePtr = this.mod.HEAPU32[(base + COL_NAME_PTR_OFF) >> 2];
    const nameLen = this.mod.HEAPU32[(base + COL_NAME_LEN_OFF) >> 2];
    if (!namePtr || nameLen === 0) return "";
    return textDecoder.decode(this.mod.HEAPU8.subarray(namePtr, namePtr + nameLen));
  }

  getColumnType(col: number): ZephyrType {
    const colsPtr = this.mod.HEAPU32[(this.ptr + RS_COLUMNS_PTR_OFF) >> 2];
    const base = colsPtr + col * COL_DESC_SIZE;
    return this.mod.HEAP32[(base + COL_TYPE_OFF) >> 2] as ZephyrType;
  }

  private cellBase(row: number, col: number): number {
    const valuesPtr = this.mod.HEAPU32[(this.ptr + RS_VALUES_PTR_OFF) >> 2];
    return valuesPtr + (row * this.colCount + col) * VAL_SIZE;
  }

  getCellType(row: number, col: number): ZephyrType {
    const base = this.cellBase(row, col);
    return this.mod.HEAP32[(base + VAL_TYPE_OFF) >> 2] as ZephyrType;
  }

  getInt64(row: number, col: number): bigint {
    const base = this.cellBase(row, col);
    return this.view().getBigInt64(base + VAL_UNION_OFF, true);
  }

  getFloat64(row: number, col: number): number {
    const base = this.cellBase(row, col);
    return this.mod.HEAPF64[(base + VAL_UNION_OFF) >> 3];
  }

  getBoolean(row: number, col: number): boolean {
    const base = this.cellBase(row, col);
    return this.mod.HEAPU8[base + VAL_UNION_OFF] !== 0;
  }

  /** Zero-copy view into WASM linear memory for TEXT/BYTEA cells. */
  getBytesView(row: number, col: number): Uint8Array {
    const base = this.cellBase(row, col);
    const slicePtr = this.mod.HEAPU32[(base + VAL_UNION_OFF) >> 2];
    const sliceLen = this.mod.HEAPU32[(base + VAL_UNION_OFF + 4) >> 2];
    if (!slicePtr || sliceLen === 0) return new Uint8Array(0);
    return this.mod.HEAPU8.subarray(slicePtr, slicePtr + sliceLen);
  }

  getString(row: number, col: number): string {
    const view = this.getBytesView(row, col);
    return view.length === 0 ? "" : textDecoder.decode(view);
  }

  free(): void {
    if (this.ptr) {
      this.mod._zephyr_result_free(this.ptr);
      this.ptr = 0;
    }
  }
}

export class ZephyrWasmDatabase {
  private dbPtr: number;
  private readonly mod: ZephyrWasmModule;

  private constructor(mod: ZephyrWasmModule, dbPtr: number) {
    this.mod = mod;
    this.dbPtr = dbPtr;
  }

  static open(mod: ZephyrWasmModule, dataDir = "/zephyr_mem"): ZephyrWasmDatabase {
    const dirBytes = mod.lengthBytesUTF8(dataDir) + 1;
    const dirPtr = mod._malloc(dirBytes);
    const cfgPtr = mod._malloc(CFG_SIZE);
    try {
      mod.stringToUTF8(dataDir, dirPtr, dirBytes);
      mod.HEAPU8.fill(0, cfgPtr, cfgPtr + CFG_SIZE);
      mod.HEAPU32[cfgPtr >> 2] = dirPtr;
      const view = new DataView(mod.HEAPU8.buffer);
      view.setBigUint64(cfgPtr + 8, 65536n, true);
      mod.HEAPU32[(cfgPtr + 16) >> 2] = 10;
      mod.HEAPU32[(cfgPtr + 20) >> 2] = 0; // WAL disabled in browser memory mode
      mod.HEAPU32[(cfgPtr + 24) >> 2] = 0;

      const dbPtr = mod._zephyr_db_open(cfgPtr);
      if (!dbPtr) {
        throw new Error(`Failed to open ZephyrDB WASM instance at ${dataDir}`);
      }
      return new ZephyrWasmDatabase(mod, dbPtr);
    } finally {
      mod._free(cfgPtr);
      mod._free(dirPtr);
    }
  }

  private withCString<T>(s: string, fn: (ptr: number) => T): T {
    const len = this.mod.lengthBytesUTF8(s) + 1;
    const ptr = this.mod._malloc(len);
    try {
      this.mod.stringToUTF8(s, ptr, len);
      return fn(ptr);
    } finally {
      this.mod._free(ptr);
    }
  }

  exec(sql: string): number {
    return this.withCString(sql, (sqlPtr) => {
      const affectedPtr = this.mod._malloc(8);
      try {
        const status = this.mod._zephyr_exec(this.dbPtr, sqlPtr, affectedPtr) as ZephyrStatus;
        if (status !== ZephyrStatus.Ok) {
          throw new Error(this.lastError());
        }
        const view = new DataView(this.mod.HEAPU8.buffer);
        return Number(view.getBigUint64(affectedPtr, true));
      } finally {
        this.mod._free(affectedPtr);
      }
    });
  }

  query(sql: string): ZephyrWasmResult {
    return this.withCString(sql, (sqlPtr) => {
      const rsPtr = this.mod._zephyr_query(this.dbPtr, sqlPtr);
      if (!rsPtr) {
        throw new Error(this.lastError());
      }
      const res = new ZephyrWasmResult(this.mod, rsPtr);
      if (res.status !== ZephyrStatus.Ok) {
        const msg = this.lastError();
        res.free();
        throw new Error(msg);
      }
      return res;
    });
  }

  lastError(): string {
    const p = this.mod._zephyr_last_error(this.dbPtr);
    return p ? this.mod.UTF8ToString(p) : "";
  }

  close(): void {
    if (this.dbPtr) {
      this.mod._zephyr_db_close(this.dbPtr);
      this.dbPtr = 0;
    }
  }
}
