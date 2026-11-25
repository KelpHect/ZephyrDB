using System;
using System.Runtime.InteropServices;

namespace ZephyrDB.Native
{
    public enum ZephyrStatus : int
    {
        Ok = 0,
        ErrInvalidArg = 1,
        ErrSyntax = 2,
        ErrNotFound = 3,
        ErrConstraint = 4,
        ErrIo = 5,
        ErrInternal = 6,
        ErrBusy = 7
    }

    public enum ZephyrType : int
    {
        Null = 0,
        I64 = 1,
        F64 = 2,
        Bool = 3,
        Text = 4,
        Bytea = 5
    }

    [Flags]
    public enum ZephyrResultFlags : uint
    {
        None = 0,
        O1FastPath = 1u << 0,
        Returning = 1u << 1
    }

    /* 16 bytes on 64-bit platforms (ptr @ 0, len @ 8, reserved @ 12) */
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ZephyrSlice
    {
        public byte* Ptr;
        public uint Len;
        public uint Reserved;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct ZephyrValueUnion
    {
        [FieldOffset(0)] public long I64;
        [FieldOffset(0)] public double F64;
        [FieldOffset(0)] public byte Boolean;
        [FieldOffset(0)] public ZephyrSlice Slice;
    }

    /* 24 bytes on 64-bit platforms (type @ 0, _pad @ 4, as @ 8) */
    [StructLayout(LayoutKind.Sequential)]
    public struct ZephyrValue
    {
        public int Type;
        public uint Pad;
        public ZephyrValueUnion As;
    }

    /* 16 bytes on 64-bit platforms (name @ 0, name_len @ 8, type @ 12) */
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ZephyrColumnDesc
    {
        public byte* Name;
        public uint NameLen;
        public int Type;
    }

    /* 56 bytes on 64-bit platforms */
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ZephyrResultSet
    {
        public ulong RowCount;
        public ulong ColCount;
        public ulong RowsAffected;
        public ZephyrColumnDesc* Columns;
        public ZephyrValue* Values;
        public void* ArenaHandle;
        public uint Status;
        public uint Flags;
    }

    /* 32 bytes */
    [StructLayout(LayoutKind.Sequential)]
    public struct ZephyrAggregate
    {
        public long Count;
        public long SumI64;
        public double SumF64;
        public int Status;
        public uint Pad;
    }

    /* 32 bytes on 64-bit platforms */
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ZephyrConfig
    {
        public byte* DataDir;
        public ulong ArenaSlabBytes;
        public uint WalFlushMs;
        public uint EnableWal;
        public uint EnableCompactor;
        public uint Pad;
    }

    public static unsafe class ZephyrNative
    {
        private const string LibName = "zephyrdb";

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ZephyrConfig zephyr_default_config();

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr zephyr_db_open(ZephyrConfig* config);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void zephyr_db_close(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int zephyr_exec(
            IntPtr db,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string sql,
            ulong* rowsAffected);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ZephyrResultSet* zephyr_query(
            IntPtr db,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string sql);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ZephyrResultSet* zephyr_query_params(
            IntPtr db,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string sql,
            ZephyrValue* @params,
            uint paramCount);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void zephyr_result_free(ZephyrResultSet* result);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ZephyrAggregate zephyr_aggregate_count(
            IntPtr db,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tableName);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern ZephyrAggregate zephyr_aggregate_sum(
            IntPtr db,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tableName,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string colName);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int zephyr_checkpoint(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern byte* zephyr_last_error(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern byte* zephyr_result_to_json(
            IntPtr db,
            ZephyrResultSet* result,
            uint* outLen);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int zephyr_raft_start(
            IntPtr db,
            uint nodeId,
            ushort port,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string peersCsv);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void zephyr_raft_stop(IntPtr db);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int zephyr_pgwire_start(IntPtr db, ushort port);

        [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void zephyr_pgwire_stop(IntPtr db);
    }
}
