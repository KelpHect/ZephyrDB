# Unity C# Guide (Zero-GC)

ZephyrDB is engineered to run inside a Unity frame loop without triggering Garbage Collection spikes. Every query result is packed into a 64-byte aligned native `Arena` (`ZephyrResultSet`), and `ZephyrDB.cs` exposes `unsafe` `ReadOnlySpan<byte>` views directly over native memory.

See [Zero-Copy C-ABI](abi.md) for exact struct byte offsets (`ZephyrSlice`, `ZephyrValue`, `ZephyrColumnDesc`, `ZephyrResultSet`).

---

## 1. Installation in Unity

1. Build the native shared library (`zephyrdb.dll` on Windows, `libzephyrdb.so` on Linux, `libzephyrdb.dylib` on macOS).
2. Copy the native binary into `Assets/Plugins/<Platform>/`.
3. Copy `bindings/unity/ZephyrNative.cs` and `bindings/unity/ZephyrDB.cs` into `Assets/Scripts/ZephyrDB/`.
4. Enable **Allow 'unsafe' Code** in **Project Settings → Player → Other Settings**.

---

## 2. Zero-GC Query Pattern (`ZephyrDatabase` & `ReadOnlySpan<byte>`)

`ZephyrDatabase`, `ZephyrResult`, and `ZephyrRowRef` provide a safe `IDisposable` wrapper over `ZephyrNative`:

```csharp
using System;
using System.Text;
using UnityEngine;
using ZephyrDB;

public class NpcMemoryBehaviour : MonoBehaviour
{
    private ZephyrDatabase _db;

    void Awake()
    {
        _db = ZephyrDatabase.Open(Application.persistentDataPath + "/zephyr_save");
        _db.Exec("CREATE TABLE IF NOT EXISTS npcs (id BIGINT PRIMARY KEY, name TEXT, hp DOUBLE PRECISION, hostility BIGINT);");
        _db.Exec("INSERT INTO npcs VALUES (1, 'Guard', 100.5, 2);");
    }

    void Update()
    {
        // Zero-GC query: native Arena memory is read in-place via ReadOnlySpan<byte>
        using var res = _db.Query("SELECT id, name, hp FROM npcs ORDER BY id;");
        for (int i = 0; i < res.RowCount; i++)
        {
            ZephyrRowRef row = res.GetRow(i);
            long id = row.GetInt64(0);
            double hp = row.GetDouble(2);

            // Points directly into ZephyrResultSet's native Arena tail (0 heap allocations)
            ReadOnlySpan<byte> utf8Name = row.GetUtf8Span(1);
            // Parse or compare utf8Name directly without calling row.GetString(1)
        }
    }

    void OnDestroy()
    {
        _db?.Dispose();
    }
}
```

> **Tip:** Avoid calling `row.GetString(col)` in `Update()` when you only need to compare tags or write into a `TMP_Text` / `char[]` buffer—use `row.GetUtf8Span(col)` (`ReadOnlySpan<byte>`) for zero managed heap allocations.

---

## 3. Direct O(1) Aggregate Probes

Skip SQL parsing altogether for frame-rate telemetry or AI blackboard counters using `AggregateCount` and `AggregateSum`:

```csharp
var countAgg = _db.AggregateCount("npcs");
var sumAgg = _db.AggregateSum("npcs", "hostility");
```

Both map directly to `ZephyrNative.zephyr_aggregate_count` and `ZephyrNative.zephyr_aggregate_sum` and return a 32-byte `ZephyrAggregate` struct by value in `O(1)` time.

---

## 4. Verifying P/Invoke Layouts with .NET

ZephyrDB includes an automated .NET test project (`bindings/unity/ZephyrDB.Tests`) that asserts `Marshal.SizeOf` / `OffsetOf` for every struct in `ZephyrNative.cs` and runs a live P/Invoke roundtrip against `zephyrdb.dll`:

```powershell
$env:PATH = "$PWD\build;$env:PATH"
dotnet run -c Release --project bindings/unity/ZephyrDB.Tests
```
