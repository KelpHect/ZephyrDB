using System;
using System.Runtime.InteropServices;
using System.Text;
using ZephyrDB.Native;

namespace ZephyrDB
{
    public sealed class ZephyrException : Exception
    {
        public ZephyrStatus Status { get; }

        public ZephyrException(ZephyrStatus status, string message)
            : base(message)
        {
            Status = status;
        }
    }

    /// <summary>
    /// Zero-copy view over a single row inside a native ZephyrResultSet.
    /// Valid only while the parent ZephyrResult is alive.
    /// </summary>
    public readonly unsafe struct ZephyrRowRef
    {
        private readonly ZephyrValue* _cells;
        private readonly int _colCount;

        internal ZephyrRowRef(ZephyrValue* cells, int colCount)
        {
            _cells = cells;
            _colCount = colCount;
        }

        public int ColumnCount => _colCount;

        public ZephyrType GetColumnType(int col)
        {
            CheckCol(col);
            return (ZephyrType)_cells[col].Type;
        }

        public bool IsNull(int col)
        {
            CheckCol(col);
            return (ZephyrType)_cells[col].Type == ZephyrType.Null;
        }

        public long GetInt64(int col)
        {
            CheckCol(col);
            return _cells[col].As.I64;
        }

        public double GetDouble(int col)
        {
            CheckCol(col);
            return _cells[col].As.F64;
        }

        public bool GetBoolean(int col)
        {
            CheckCol(col);
            return _cells[col].As.Boolean != 0;
        }

        public ReadOnlySpan<byte> GetUtf8Span(int col)
        {
            CheckCol(col);
            ZephyrSlice slice = _cells[col].As.Slice;
            if (slice.Ptr == null || slice.Len == 0)
            {
                return ReadOnlySpan<byte>.Empty;
            }
            return new ReadOnlySpan<byte>(slice.Ptr, checked((int)slice.Len));
        }

        public ReadOnlySpan<byte> GetBytesSpan(int col) => GetUtf8Span(col);

        public string GetString(int col)
        {
            var span = GetUtf8Span(col);
            return span.IsEmpty ? string.Empty : Encoding.UTF8.GetString(span);
        }

        private void CheckCol(int col)
        {
            if ((uint)col >= (uint)_colCount)
            {
                throw new ArgumentOutOfRangeException(nameof(col));
            }
        }
    }

    /// <summary>
    /// Safe disposable wrapper around a native arena-allocated ZephyrResultSet*.
    /// </summary>
    public sealed unsafe class ZephyrResult : IDisposable
    {
        private ZephyrResultSet* _ptr;

        internal ZephyrResult(ZephyrResultSet* ptr)
        {
            _ptr = ptr;
        }

        public int RowCount => _ptr != null ? checked((int)_ptr->RowCount) : 0;
        public int ColCount => _ptr != null ? checked((int)_ptr->ColCount) : 0;
        public ulong RowsAffected => _ptr != null ? _ptr->RowsAffected : 0;
        public ZephyrStatus Status => _ptr != null ? (ZephyrStatus)_ptr->Status : ZephyrStatus.ErrInternal;
        public bool IsO1FastPath => _ptr != null && ((_ptr->Flags & (uint)ZephyrResultFlags.O1FastPath) != 0);

        public ReadOnlySpan<byte> GetColumnNameSpan(int col)
        {
            if (_ptr == null || (uint)col >= (uint)_ptr->ColCount)
            {
                throw new ArgumentOutOfRangeException(nameof(col));
            }
            ZephyrColumnDesc desc = _ptr->Columns[col];
            if (desc.Name == null || desc.NameLen == 0)
            {
                return ReadOnlySpan<byte>.Empty;
            }
            return new ReadOnlySpan<byte>(desc.Name, checked((int)desc.NameLen));
        }

        public string GetColumnName(int col)
        {
            var span = GetColumnNameSpan(col);
            return span.IsEmpty ? string.Empty : Encoding.UTF8.GetString(span);
        }

        public ZephyrType GetColumnType(int col)
        {
            if (_ptr == null || (uint)col >= (uint)_ptr->ColCount)
            {
                throw new ArgumentOutOfRangeException(nameof(col));
            }
            return (ZephyrType)_ptr->Columns[col].Type;
        }

        public ZephyrRowRef GetRow(int row)
        {
            if (_ptr == null || (uint)row >= (uint)_ptr->RowCount)
            {
                throw new ArgumentOutOfRangeException(nameof(row));
            }
            int cols = checked((int)_ptr->ColCount);
            return new ZephyrRowRef(_ptr->Values + (row * cols), cols);
        }

        internal ZephyrResultSet* DangerousGetPointer() => _ptr;

        public void Dispose()
        {
            if (_ptr != null)
            {
                ZephyrNative.zephyr_result_free(_ptr);
                _ptr = null;
            }
            GC.SuppressFinalize(this);
        }

        ~ZephyrResult()
        {
            if (_ptr != null)
            {
                ZephyrNative.zephyr_result_free(_ptr);
                _ptr = null;
            }
        }
    }

    /// <summary>
    /// Managed wrapper for a ZephyrDB instance.
    /// </summary>
    public sealed unsafe class ZephyrDatabase : IDisposable
    {
        private IntPtr _db;

        private ZephyrDatabase(IntPtr db)
        {
            _db = db;
        }

        public static ZephyrDatabase Open(
            string dataDir,
            ulong arenaSlabBytes = 65536,
            uint walFlushMs = 10,
            bool enableWal = true,
            bool enableCompactor = true)
        {
            ZephyrConfig cfg = ZephyrNative.zephyr_default_config();
            cfg.ArenaSlabBytes = arenaSlabBytes;
            cfg.WalFlushMs = walFlushMs;
            cfg.EnableWal = enableWal ? 1u : 0u;
            cfg.EnableCompactor = enableCompactor ? 1u : 0u;

            byte[] dirBytes = Encoding.UTF8.GetBytes(dataDir + "\0");
            fixed (byte* pDir = dirBytes)
            {
                cfg.DataDir = pDir;
                IntPtr db = ZephyrNative.zephyr_db_open(&cfg);
                if (db == IntPtr.Zero)
                {
                    throw new ZephyrException(ZephyrStatus.ErrIo, $"Failed to open ZephyrDB at '{dataDir}'");
                }
                return new ZephyrDatabase(db);
            }
        }

        public ulong Exec(string sql)
        {
            ThrowIfDisposed();
            ulong affected = 0;
            var status = (ZephyrStatus)ZephyrNative.zephyr_exec(_db, sql, &affected);
            if (status != ZephyrStatus.Ok)
            {
                throw new ZephyrException(status, LastError());
            }
            return affected;
        }

        public ZephyrResult Query(string sql)
        {
            ThrowIfDisposed();
            ZephyrResultSet* rs = ZephyrNative.zephyr_query(_db, sql);
            if (rs == null)
            {
                throw new ZephyrException(ZephyrStatus.ErrInternal, LastError());
            }
            if ((ZephyrStatus)rs->Status != ZephyrStatus.Ok)
            {
                var st = (ZephyrStatus)rs->Status;
                ZephyrNative.zephyr_result_free(rs);
                throw new ZephyrException(st, LastError());
            }
            return new ZephyrResult(rs);
        }

        public ZephyrAggregate AggregateCount(string tableName)
        {
            ThrowIfDisposed();
            return ZephyrNative.zephyr_aggregate_count(_db, tableName);
        }

        public ZephyrAggregate AggregateSum(string tableName, string colName)
        {
            ThrowIfDisposed();
            return ZephyrNative.zephyr_aggregate_sum(_db, tableName, colName);
        }

        public void Checkpoint()
        {
            ThrowIfDisposed();
            var status = (ZephyrStatus)ZephyrNative.zephyr_checkpoint(_db);
            if (status != ZephyrStatus.Ok)
            {
                throw new ZephyrException(status, LastError());
            }
        }

        public string LastError()
        {
            if (_db == IntPtr.Zero) return "Database closed";
            byte* p = ZephyrNative.zephyr_last_error(_db);
            return p == null ? string.Empty : (Marshal.PtrToStringUTF8((IntPtr)p) ?? string.Empty);
        }

        public void StartRaft(uint nodeId, ushort port, string peersCsv)
        {
            ThrowIfDisposed();
            var status = (ZephyrStatus)ZephyrNative.zephyr_raft_start(_db, nodeId, port, peersCsv);
            if (status != ZephyrStatus.Ok)
            {
                throw new ZephyrException(status, LastError());
            }
        }

        public void StopRaft()
        {
            if (_db != IntPtr.Zero) ZephyrNative.zephyr_raft_stop(_db);
        }

        public void StartPgWire(ushort port)
        {
            ThrowIfDisposed();
            var status = (ZephyrStatus)ZephyrNative.zephyr_pgwire_start(_db, port);
            if (status != ZephyrStatus.Ok)
            {
                throw new ZephyrException(status, LastError());
            }
        }

        public void StopPgWire()
        {
            if (_db != IntPtr.Zero) ZephyrNative.zephyr_pgwire_stop(_db);
        }

        private void ThrowIfDisposed()
        {
            if (_db == IntPtr.Zero)
            {
                throw new ObjectDisposedException(nameof(ZephyrDatabase));
            }
        }

        public void Dispose()
        {
            if (_db != IntPtr.Zero)
            {
                ZephyrNative.zephyr_db_close(_db);
                _db = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        ~ZephyrDatabase()
        {
            if (_db != IntPtr.Zero)
            {
                ZephyrNative.zephyr_db_close(_db);
                _db = IntPtr.Zero;
            }
        }
    }
}
