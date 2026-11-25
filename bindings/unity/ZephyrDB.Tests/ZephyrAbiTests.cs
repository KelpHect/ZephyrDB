using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using ZephyrDB;
using ZephyrDB.Native;

internal static unsafe class ZephyrAbiTests
{
    private static void AssertEqual<T>(T actual, T expected, string label) where T : IEquatable<T>
    {
        if (!actual.Equals(expected))
        {
            throw new InvalidOperationException($"Assertion failed [{label}]: expected {expected}, got {actual}");
        }
    }

    private static void TestStructLayouts()
    {
        AssertEqual(sizeof(ZephyrSlice), 16, "sizeof(ZephyrSlice)");
        AssertEqual(Marshal.OffsetOf<ZephyrSlice>(nameof(ZephyrSlice.Ptr)).ToInt32(), 0, "ZephyrSlice.Ptr");
        AssertEqual(Marshal.OffsetOf<ZephyrSlice>(nameof(ZephyrSlice.Len)).ToInt32(), 8, "ZephyrSlice.Len");
        AssertEqual(Marshal.OffsetOf<ZephyrSlice>(nameof(ZephyrSlice.Reserved)).ToInt32(), 12, "ZephyrSlice.Reserved");

        AssertEqual(sizeof(ZephyrValue), 24, "sizeof(ZephyrValue)");
        AssertEqual(Marshal.OffsetOf<ZephyrValue>(nameof(ZephyrValue.Type)).ToInt32(), 0, "ZephyrValue.Type");
        AssertEqual(Marshal.OffsetOf<ZephyrValue>(nameof(ZephyrValue.Pad)).ToInt32(), 4, "ZephyrValue.Pad");
        AssertEqual(Marshal.OffsetOf<ZephyrValue>(nameof(ZephyrValue.As)).ToInt32(), 8, "ZephyrValue.As");

        AssertEqual(sizeof(ZephyrColumnDesc), 16, "sizeof(ZephyrColumnDesc)");
        AssertEqual(Marshal.OffsetOf<ZephyrColumnDesc>(nameof(ZephyrColumnDesc.Name)).ToInt32(), 0, "ZephyrColumnDesc.Name");
        AssertEqual(Marshal.OffsetOf<ZephyrColumnDesc>(nameof(ZephyrColumnDesc.NameLen)).ToInt32(), 8, "ZephyrColumnDesc.NameLen");
        AssertEqual(Marshal.OffsetOf<ZephyrColumnDesc>(nameof(ZephyrColumnDesc.Type)).ToInt32(), 12, "ZephyrColumnDesc.Type");

        AssertEqual(sizeof(ZephyrResultSet), 56, "sizeof(ZephyrResultSet)");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.RowCount)).ToInt32(), 0, "ZephyrResultSet.RowCount");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.ColCount)).ToInt32(), 8, "ZephyrResultSet.ColCount");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.RowsAffected)).ToInt32(), 16, "ZephyrResultSet.RowsAffected");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.Columns)).ToInt32(), 24, "ZephyrResultSet.Columns");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.Values)).ToInt32(), 32, "ZephyrResultSet.Values");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.ArenaHandle)).ToInt32(), 40, "ZephyrResultSet.ArenaHandle");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.Status)).ToInt32(), 48, "ZephyrResultSet.Status");
        AssertEqual(Marshal.OffsetOf<ZephyrResultSet>(nameof(ZephyrResultSet.Flags)).ToInt32(), 52, "ZephyrResultSet.Flags");

        AssertEqual(sizeof(ZephyrAggregate), 32, "sizeof(ZephyrAggregate)");
        AssertEqual(Marshal.OffsetOf<ZephyrAggregate>(nameof(ZephyrAggregate.Count)).ToInt32(), 0, "ZephyrAggregate.Count");
        AssertEqual(Marshal.OffsetOf<ZephyrAggregate>(nameof(ZephyrAggregate.SumI64)).ToInt32(), 8, "ZephyrAggregate.SumI64");
        AssertEqual(Marshal.OffsetOf<ZephyrAggregate>(nameof(ZephyrAggregate.SumF64)).ToInt32(), 16, "ZephyrAggregate.SumF64");
        AssertEqual(Marshal.OffsetOf<ZephyrAggregate>(nameof(ZephyrAggregate.Status)).ToInt32(), 24, "ZephyrAggregate.Status");

        AssertEqual(sizeof(ZephyrConfig), 32, "sizeof(ZephyrConfig)");
        AssertEqual(Marshal.OffsetOf<ZephyrConfig>(nameof(ZephyrConfig.DataDir)).ToInt32(), 0, "ZephyrConfig.DataDir");
        AssertEqual(Marshal.OffsetOf<ZephyrConfig>(nameof(ZephyrConfig.ArenaSlabBytes)).ToInt32(), 8, "ZephyrConfig.ArenaSlabBytes");
        AssertEqual(Marshal.OffsetOf<ZephyrConfig>(nameof(ZephyrConfig.WalFlushMs)).ToInt32(), 16, "ZephyrConfig.WalFlushMs");
        AssertEqual(Marshal.OffsetOf<ZephyrConfig>(nameof(ZephyrConfig.EnableWal)).ToInt32(), 20, "ZephyrConfig.EnableWal");
        AssertEqual(Marshal.OffsetOf<ZephyrConfig>(nameof(ZephyrConfig.EnableCompactor)).ToInt32(), 24, "ZephyrConfig.EnableCompactor");

        Console.WriteLine("[PASS] All C# ABI struct sizes and offsets match c_abi.h (64-bit).");
    }

    private static void TestLiveNativeLibraryIfPresent()
    {
        if (!NativeLibrary.TryLoad("zephyrdb", out IntPtr handle))
        {
            Console.WriteLine("[SKIP] zephyrdb native library not in loader path; skipping live P/Invoke test.");
            return;
        }
        NativeLibrary.Free(handle);

        string tempDir = Path.Combine(Path.GetTempPath(), $"zephyr_cs_test_{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            using var db = ZephyrDatabase.Open(tempDir, enableWal: true, enableCompactor: false);
            db.Exec("CREATE TABLE npcs (id BIGINT PRIMARY KEY, name TEXT, hp DOUBLE PRECISION, hostile BOOLEAN);");
            db.Exec("INSERT INTO npcs (id, name, hp, hostile) VALUES (1, 'Guard', 100.5, false);");
            db.Exec("INSERT INTO npcs (id, name, hp, hostile) VALUES (2, 'Bandit', 75.0, true);");

            using var res = db.Query("SELECT id, name, hp, hostile FROM npcs ORDER BY id;");
            AssertEqual(res.RowCount, 2, "res.RowCount");
            AssertEqual(res.ColCount, 4, "res.ColCount");

            var row0 = res.GetRow(0);
            AssertEqual(row0.GetInt64(0), 1L, "row0.id");
            AssertEqual(row0.GetString(1), "Guard", "row0.name");
            AssertEqual(row0.GetBoolean(3), false, "row0.hostile");
            AssertEqual(Encoding.UTF8.GetString(row0.GetUtf8Span(1)), "Guard", "row0.utf8span");

            var agg = db.AggregateCount("npcs");
            AssertEqual(agg.Count, 2L, "agg.Count");

            Console.WriteLine("[PASS] Live zephyrdb P/Invoke zero-copy query & aggregate test passed.");
        }
        finally
        {
            if (Directory.Exists(tempDir))
            {
                Directory.Delete(tempDir, recursive: true);
            }
        }
    }

    public static int Main(string[] args)
    {
        TestStructLayouts();
        TestLiveNativeLibraryIfPresent();
        return 0;
    }
}
