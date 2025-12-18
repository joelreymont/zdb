# zdb - Zig LLDB Type Formatters

Native C++ data formatters for debugging Zig programs in LLDB. Shows slices, optionals, error unions, and standard library types in a readable format.

## Installation

```bash
cd zdb
zig build
# Output: zig-out/lib/libzdb.dylib
```

Set up the offset file for your LLDB version:

```bash
mkdir -p ~/.config/zdb/offsets
cp offsets/lldb-21.1.7.json ~/.config/zdb/offsets/
```

Add to `~/.lldbinit`:
```
plugin load /path/to/zdb/zig-out/lib/libzdb.dylib
```

## Example

Create `demo.zig`:

```zig
const std = @import("std");

const Color = enum { red, green, blue };
const Point = struct { x: i32, y: i32 };
const Shape = union(enum) {
    circle: f32,
    rectangle: struct { w: f32, h: f32 },
};

pub fn main() !void {
    const greeting: []const u8 = "Hello, zdb!";
    const numbers: []const i32 = &.{ 1, 2, 3, 4, 5 };
    var maybe: ?i32 = 42;
    var nothing: ?i32 = null;
    var color: Color = .blue;
    var point: Point = .{ .x = 100, .y = 200 };
    var shape: Shape = .{ .circle = 3.14 };

    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    var list: std.ArrayListUnmanaged(i32) = .empty;
    defer list.deinit(gpa.allocator());
    try list.appendSlice(gpa.allocator(), &.{ 10, 20, 30 });

    _ = .{ &greeting, &numbers, &maybe, &nothing, &color, &point, &shape, &list };
    std.debug.print("Ready\n", .{});  // breakpoint here
}
```

Build and debug:

```bash
zig build-exe demo.zig -femit-bin=demo
lldb demo -o "plugin load zig-out/lib/libzdb.dylib" -o "b demo.zig:25" -o run
```

Actual LLDB output with zdb:

```
(lldb) p point
(demo.Point) { .x=100, .y=200 } {
  x = 100
  y = 200
}
(lldb) p color
(demo.Color) blue .blue
(lldb) p greeting
([]u8) "Hello, zdb!" {
  ptr = 0x00000001000d166e "Hello, zdb!"
  len = 11
}
(lldb) p list
(array_list.Aligned(i32,null)) len=3 capacity=32 {
  items = len=3 ptr=0x100160000 {
    ptr = 0x0000000100160000
    len = 3
  }
  capacity = 32
}
```

## Supported Types (19 formatters)

| Pattern | Formatter | Example Output |
|---------|-----------|----------------|
| `[]u8`, `[]const u8` | String | `"Hello, World!"` |
| `[]T` | Slice | `len=5 ptr=0x100123` |
| `[N]T` | Array | `[5]...` |
| `?T` | Optional | `null` or `42` |
| `E!T` | Error Union | `error.FileNotFound` or value |
| `union(enum)` | Tagged Union | `.circle = 5.0` |
| `*T` | Pointer | `-> 42` or `null` |
| `[*]T` | Many Pointer | `0x100123456` |
| `[*:0]u8` | C String | `"null-terminated"` |
| `[*:s]T` | Sentinel Pointer | `0x100123456` |
| `module.Type` | Struct/Enum | `{ .x=1, .y=2 }` or `.blue` |
| `array_list.*` | ArrayList | `len=3 capacity=32` |
| `hash_map.*` | HashMap | `size=5` |
| `bounded_array.*` | BoundedArray | `len=10` |
| `multi_array_list.*` | MultiArrayList | `len=3 capacity=16` |
| `segmented_list.*` | SegmentedList | `len=100` |

## How It Works

LLDB's internal C++ API (`TypeCategoryImpl::AddTypeSummary`) is not exported - symbols are marked local in liblldb.dylib. zdb bypasses this via offset tables:

```
┌─────────────────────────────────────────────────────────────┐
│                      Plugin Load                            │
├─────────────────────────────────────────────────────────────┤
│  1. Parse LLDB version from SBDebugger::GetVersionString()  │
│  2. Load offset JSON from ~/.config/zdb/offsets/            │
│  3. dlopen liblldb, find reference symbol                   │
│  4. Compute base address: ref_addr - ref_offset             │
│  5. Resolve internal functions: base + offset               │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                  Formatter Registration                     │
├─────────────────────────────────────────────────────────────┤
│  For each type pattern:                                     │
│  1. SBTypeSummary::CreateWithCallback(callback_fn)          │
│  2. Extract shared_ptr from SBTypeSummary object            │
│  3. Call TypeCategoryImpl::AddTypeSummary via offset        │
│     - ARM64 ABI: shared_ptr passed indirectly (pointer)     │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                   Runtime Callback                          │
├─────────────────────────────────────────────────────────────┤
│  When LLDB displays a Zig value:                            │
│  1. Type name matches regex pattern                         │
│  2. LLDB calls our C++ callback                             │
│  3. Callback extracts fields via SBValue API                │
│  4. Writes formatted string to SBStream                     │
└─────────────────────────────────────────────────────────────┘
```

### Key Files

| File | Purpose |
|------|---------|
| `shim/shim_callback.cpp` | Plugin entry, formatters, internal API calls |
| `shim/offset_loader.h` | JSON parsing, symbol resolution |
| `offsets/lldb-*.json` | Per-version offset tables |
| `tools/dump_offsets.py` | Generate offset tables for new LLDB versions |

### ARM64 ABI Details

The tricky part is calling `TypeCategoryImpl::AddTypeSummary(StringRef, FormatterMatchType, shared_ptr<TypeSummaryImpl>)`:

- `this` pointer in x0
- `StringRef` (ptr + len) in x1, x2
- `FormatterMatchType` enum in x3
- `shared_ptr` passed **indirectly** in x4 (pointer to 16-byte struct)

Non-trivial types like `shared_ptr` (with destructor) are passed by pointer on ARM64, not inline in registers.

## Creating Offset Tables for New LLDB Versions

zdb requires an offset table matching your LLDB version. When LLDB updates, generate a new table:

```bash
# Check your LLDB version
lldb --version
# lldb version 21.1.7

# Generate offset table
python3 tools/dump_offsets.py /opt/homebrew/opt/llvm/lib/liblldb.dylib > offsets/lldb-21.1.7.json

# Install it
mkdir -p ~/.config/zdb/offsets
cp offsets/lldb-21.1.7.json ~/.config/zdb/offsets/
```

The tool uses `nm` to find internal LLDB symbols and calculates their offsets relative to an exported reference symbol (`SBDebugger::Initialize`). At runtime, zdb:

1. Finds the reference symbol via `dlsym`
2. Computes base address: `ref_addr - ref_offset`
3. Resolves internal functions: `base + symbol_offset`

**Common LLDB paths:**
- macOS Homebrew: `/opt/homebrew/opt/llvm/lib/liblldb.dylib`
- macOS Xcode: `/Applications/Xcode.app/.../liblldb.dylib`
- Linux: `/usr/lib/liblldb.so`

**If `dump_offsets.py` shows warnings:** Some symbols may not exist in your LLDB version. The core symbols (`GetCategory`, `AddTypeSummary`, `Enable`) are required.

## Testing

```bash
# Run automated tests
./test/run_tests.sh

# Manual testing
lldb test/test_types \
    -o "plugin load zig-out/lib/libzdb.dylib" \
    -o "b test_types.zig:157" \
    -o "run" \
    -o "frame variable"
```

Tests verify: string slices, int slices, enums, structs, ArrayList, HashMap.

## Expression Evaluation

zdb transparently extends the `p` command to support native Zig syntax:

```
(lldb) p int_slice[0]
(int) $0 = 1

(lldb) p int_slice[2]
(int) $1 = 3

(lldb) p list[0]
(int) $2 = 10

(lldb) p test_struct.optional_value.?
(int) $3 = 42

(lldb) p test_struct.error_result catch 0
(int) $4 = 100

(lldb) p string_slice
([]u8) "Hello, zdb debugger!"
```

**Supported Zig syntax:**
| Syntax | Transformation | Example |
|--------|---------------|---------|
| `slice[n]` | `slice.ptr[n]` | `p my_slice[0]` |
| `arraylist[n]` | `arraylist.items.ptr[n]` | `p list[0]` |
| `optional.?` | `optional.data` | `p maybe_value.?` |
| `err catch default` | `(err.tag == 0 ? err.value : default)` | `p result catch 0` |

All transformations are automatic and transparent - just use `p` as usual.

## Comparison with zig-lldb

| Feature | zdb | zig-lldb |
|---------|-----|----------|
| Installation | Plugin, no rebuild | Rebuild LLDB from source |
| Type formatting | ✓ | ✓ |
| `p slice[n]` | ✓ | ✓ |
| `p optional.?` | ✓ | ✓ |
| `p err catch val` | ✓ | ✓ |
| Variables view children | ✗ | ✓ |
| Full TypeSystem | ✗ | ✓ |
| Works with stock LLDB | ✓ | ✗ |

**zdb** provides type formatters and Zig expression syntax via automatic transformation. No LLDB rebuild required.

**zig-lldb** ([Jacob Shtoyer's fork](https://github.com/jacobly0/llvm-project/tree/lldb-zig)) implements a full `TypeSystemZig` with deep DWARF integration. Requires ~1hr to rebuild LLDB from source.

## Limitations

**Variables View Expansion**: IDE variables view cannot expand slices to show `[0]`, `[1]`, ... elements. This requires LLDB synthetic children providers, which cannot be registered from a plugin due to C++ ABI barriers (`std::function` type erasure incompatibility between plugin and LLDB builds).

**Workaround**: Use the `p` command with Zig syntax:
```
(lldb) p my_slice[0]    # Works!
(lldb) p my_slice[5]    # Works!
```

This limitation does not affect zig-lldb, which implements a full TypeSystem with native DWARF integration.

## License

MIT
