# zdb - Zig LLDB Language Plugin

A Zig-native LLDB language plugin for improved Zig debugging experience.

## Goal

Create a dylib that LLDB can load via `plugin load ./libzdb.dylib` to provide:
- Type formatters for Zig types (slices, optionals, error unions, tagged unions)
- Synthetic children providers for std library types
- Better variable display in debugger

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         LLDB                                │
│  (loads libzdb.dylib via `plugin load`)                     │
├─────────────────────────────────────────────────────────────┤
│                    C++ Shim (shim.cpp)                      │
│  - Implements LLDB's TypeSystem/LanguageRuntime interfaces  │
│  - Delegates to Zig via extern "C" function pointers        │
│  - Handles C++ ↔ Zig type conversions                       │
├─────────────────────────────────────────────────────────────┤
│                    Zig Core (src/zdb.zig)                   │
│  - Type parsing and formatting logic                        │
│  - Synthetic children generation                            │
│  - DWARF interpretation for Zig types                       │
└─────────────────────────────────────────────────────────────┘
```

## Build

```bash
zig build              # Build debug
zig build -Doptimize=ReleaseFast  # Build release
```

Output: `zig-out/lib/libzdb.dylib` (macOS) or `libzdb.so` (Linux)

## Usage

```
$ lldb ./my_zig_program
(lldb) plugin load ./zig-out/lib/libzdb.dylib
(lldb) run
```

## Project Structure

```
zdb/
├── AGENTS.md           # This file
├── build.zig           # Zig build system
├── src/
│   ├── zdb.zig         # Main Zig implementation
│   ├── formatters.zig  # Type formatter implementations
│   ├── synthetic.zig   # Synthetic children providers
│   └── dwarf.zig       # DWARF parsing utilities
├── shim/
│   ├── shim.cpp        # C++ shim for LLDB plugin API
│   ├── shim.h          # C interface between shim and Zig
│   └── CMakeLists.txt  # For building shim (optional)
└── test/
    ├── test_program.zig  # Test Zig program to debug
    └── test_formatters.zig
```

## Implementation Plan

### Phase 1: Minimal Viable Plugin

1. **Set up build system**
   - build.zig that compiles Zig code to a static library
   - Link with C++ shim to produce final dylib
   - Need LLDB headers (from Homebrew or LLVM source)

2. **Create C++ shim**
   - Implement `lldb::PluginInterface`
   - Register a `TypeSummaryProvider` for basic Zig types
   - Export `extern "C"` functions that Zig calls

3. **Implement basic formatters in Zig**
   - `[]T` slices → show `ptr` and `len`, expand children
   - `?T` optionals → show "null" or unwrapped value
   - `E!T` error unions → show error name or payload

### Phase 2: Synthetic Children

4. **Synthetic providers for complex types**
   - `std.ArrayList` → show items array
   - `std.HashMap` → show key/value pairs
   - `std.MultiArrayList` → show struct-of-arrays view

5. **Tagged union support**
   - Parse DWARF to find active tag
   - Display only active variant

### Phase 3: Advanced Features

6. **Expression evaluation hooks**
   - Allow `p slice[5]` to work correctly
   - Support `.?` and `.!` operators

7. **Frame variable improvements**
   - Better display of function arguments
   - Comptime values

## Key Technical Challenges

### 1. LLDB Plugin API

LLDB's plugin API is internal C++ with no stable ABI. Need to:
- Match exact LLDB version (check with `lldb --version`)
- Use same compiler/stdlib LLDB was built with
- Handle vtable layout differences

**LLDB headers location (Homebrew):**
```
/opt/homebrew/opt/llvm/include/lldb/
```

**LLDB library:**
```
/opt/homebrew/opt/llvm/lib/liblldb.dylib
```

### 2. Type Summary vs Synthetic Children

LLDB has two formatter mechanisms:

**TypeSummaryProvider** - Returns a string summary:
```cpp
class ZigSliceSummary : public TypeSummaryImpl {
    bool FormatObject(ValueObject *valobj, std::string &dest) override {
        // Return "[]u8 len=42 ptr=0x1234"
    }
};
```

**SyntheticChildrenFrontEnd** - Provides expandable children:
```cpp
class ZigSliceSynthetic : public SyntheticChildrenFrontEnd {
    size_t CalculateNumChildren() override { return len; }
    ValueObjectSP GetChildAtIndex(size_t idx) override { ... }
};
```

### 3. DWARF Type Information

Zig emits DWARF with custom extensions. Key tags:
- `DW_TAG_structure_type` for slices (has `ptr` and `len` members)
- `DW_TAG_union_type` for optionals/error unions
- Custom attributes for Zig-specific info

Jacob's fork adds: `DW_AT_LLVM_zig_*` attributes.

### 4. C++ Shim Design

The shim needs to:

```cpp
// shim.h - C interface
extern "C" {
    // Called by Zig to register formatters
    void zdb_register_summary(const char* type_regex, ZdbFormatFn format_fn);
    void zdb_register_synthetic(const char* type_regex, ZdbSyntheticVTable* vtable);

    // Called by shim into Zig
    typedef bool (*ZdbFormatFn)(void* valobj_opaque, char* out_buf, size_t buf_size);

    typedef struct {
        size_t (*num_children)(void* valobj_opaque);
        void* (*get_child)(void* valobj_opaque, size_t index);
        const char* (*get_child_name)(void* valobj_opaque, size_t index);
    } ZdbSyntheticVTable;
}
```

```cpp
// shim.cpp
#include "lldb/DataFormatters/TypeSummary.h"

class ZigSummaryProvider : public TypeSummaryImpl {
    ZdbFormatFn m_zig_fn;
public:
    bool FormatObject(ValueObject *valobj, std::string &dest) override {
        char buf[1024];
        if (m_zig_fn(valobj, buf, sizeof(buf))) {
            dest = buf;
            return true;
        }
        return false;
    }
};
```

### 5. ValueObject Access

LLDB's `ValueObject` is how you read debugee memory. The shim must expose:

```cpp
// Functions Zig needs to call (via shim)
extern "C" {
    uint64_t zdb_valobj_get_uint(void* valobj, const char* child_name);
    void* zdb_valobj_get_child(void* valobj, const char* child_name);
    void* zdb_valobj_get_child_at_index(void* valobj, size_t index);
    const char* zdb_valobj_get_type_name(void* valobj);
    size_t zdb_valobj_read_memory(void* valobj, uint64_t addr, void* buf, size_t size);
}
```

## Dependencies

- **Zig 0.14+** - For building Zig code
- **LLVM/LLDB headers** - Match your LLDB version
- **C++ compiler** - clang++ (same as LLDB was built with)

### macOS (Homebrew)

```bash
brew install llvm
export LLVM_PATH=/opt/homebrew/opt/llvm
```

### Linux

```bash
apt install lldb-18 liblldb-18-dev
export LLVM_PATH=/usr/lib/llvm-18
```

## References

- [Jacob's LLDB fork](https://github.com/jacobly0/llvm-project/tree/lldb-zig) - Reference implementation
- [RFC: Upstreaming Zig to LLDB](https://discourse.llvm.org/t/rfc-upstreaming-zig-language-support-to-lldb/88127)
- [Zig pretty printers](https://github.com/ziglang/zig/blob/master/tools/lldb_pretty_printers.py) - Python version
- [LLDB Data Formatters](https://lldb.llvm.org/use/variable.html) - Official docs
- [LLDB Plugin Architecture](https://lldb.llvm.org/design/plugins.html) - How plugins work

## Getting Started

1. **Read Jacob's fork** to understand what DWARF changes Zig makes:
   ```bash
   git clone https://github.com/jacobly0/llvm-project.git --branch lldb-zig --depth 1
   # Look at: lldb/source/Plugins/Language/Zig/
   ```

2. **Study the Python pretty printers** to understand Zig type layouts:
   ```bash
   curl -O https://raw.githubusercontent.com/ziglang/zig/master/tools/lldb_pretty_printers.py
   ```

3. **Build a minimal shim** that just logs when loaded:
   ```cpp
   // shim.cpp
   #include <cstdio>
   extern "C" void __attribute__((constructor)) zdb_init() {
       fprintf(stderr, "zdb plugin loaded!\n");
   }
   ```

4. **Test loading**:
   ```bash
   clang++ -shared -fPIC shim.cpp -o libzdb.dylib
   lldb -o "plugin load ./libzdb.dylib" -o "quit"
   ```

## Current Status

- [x] Project structure created
- [x] build.zig working (Zig 0.15 API)
- [x] C++ shim compiles with LLDB headers
- [x] Plugin loads in LLDB successfully
- [ ] Basic slice formatter
- [ ] Optional formatter
- [ ] Error union formatter
- [ ] std.ArrayList synthetic
- [ ] std.HashMap synthetic

## Quick Test

```bash
cd /Users/joel/Work/zdb
zig build
lldb -o "plugin load ./zig-out/lib/libzdb.dylib" -o "quit"
# Should print: [zdb] PluginInitialize called
```

## Notes

- LLDB plugin API changes between versions - pin to specific LLVM version
- macOS requires code signing for debugger plugins in some cases
- Test with both `-fno-llvm` (self-hosted) and LLVM backend builds
