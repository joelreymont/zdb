#!/bin/bash
# Automated tests for zdb formatters
set -e

cd "$(dirname "$0")/.."

# Find LLDB - prefer Homebrew LLVM (has offset files), then env vars, then PATH
find_lldb() {
    # Explicit override
    if [ -n "$LLDB" ] && [ -x "$LLDB" ]; then
        echo "$LLDB"
        return 0
    fi
    # LLVM_PATH env var
    if [ -n "$LLVM_PATH" ] && [ -x "$LLVM_PATH/bin/lldb" ]; then
        echo "$LLVM_PATH/bin/lldb"
        return 0
    fi
    # Homebrew LLVM first (we have offset files for this)
    for path in /opt/homebrew/opt/llvm/bin/lldb /usr/local/opt/llvm/bin/lldb; do
        if [ -x "$path" ]; then
            echo "$path"
            return 0
        fi
    done
    # Fallback to PATH (may be Apple LLDB without offset files)
    if command -v lldb >/dev/null 2>&1; then
        command -v lldb
        return 0
    fi
    return 1
}

LLDB=$(find_lldb) || {
    echo "ERROR: LLDB not found. Set LLDB or LLVM_PATH environment variable."
    exit 1
}

# Build plugin
echo "Building plugin..."
zig build

# Build test program
echo "Building test program..."
(cd test && zig build-exe test_types.zig -femit-bin=test_types -fno-strip 2>/dev/null)

# Find offset file
OFFSET_FILE="${ZDB_OFFSETS_FILE:-$HOME/.config/zdb/offsets/lldb-21.1.7.json}"
if [ ! -f "$OFFSET_FILE" ]; then
    OFFSET_FILE="offsets/lldb-21.1.7.json"
fi

if [ ! -f "$OFFSET_FILE" ]; then
    echo "ERROR: No offset file found"
    exit 1
fi

export ZDB_OFFSETS_FILE="$OFFSET_FILE"

echo "Running formatter tests with $LLDB..."

# Capture LLDB output - test formatters and expression syntax
OUTPUT=$("$LLDB" test/test_types \
    -o "plugin load zig-out/lib/libzdb.dylib" \
    -o "b test_types.zig:157" \
    -o "run" \
    -o "frame variable" \
    -o "p int_slice[0]" \
    -o "p int_slice[2]" \
    -o "p list[0]" \
    -o "p test_struct.optional_value.?" \
    -o "p test_struct.error_result catch 0" \
    -o "quit" 2>&1)

FAILED=0

check() {
    local name="$1"
    local pattern="$2"
    if echo "$OUTPUT" | grep -qE "$pattern"; then
        echo "✓ $name"
    else
        echo "✗ $name - expected: $pattern"
        FAILED=1
    fi
}

# Check formatters loaded
check "Plugin loaded" "\[zdb\] Loaded [0-9]+ formatters"

# Test string formatters
check "String slice" 'string_slice = "Hello, zdb debugger!"'
check "Byte slice" 'byte_slice = "abcde"'

# Test slice formatter
check "Int slice" 'int_slice = len=5 ptr='

# Test enum formatter (shows "blue .blue" - native enum value + our .prefix)
check "Enum" 'color = blue \.blue'

# Test struct formatter (test_struct has 5 fields)
check "Struct" 'test_struct = \{ 5 fields \}'

# Test std library types
check "ArrayList" 'list = len=3'
check "HashMap" 'map = size=3'

# Test Zig expression syntax (transparent via 'p' command)
check "Expr: slice[n]" '\(int\).*= 1'
check "Expr: slice[2]" '\(int\).*= 3'
check "Expr: arraylist[n]" '\(int\).*= 10'
check "Expr: optional.?" '\(int\).*= 42'
check "Expr: err catch" '\(int\).*= 100'

echo ""
if [ $FAILED -eq 0 ]; then
    echo "All tests passed!"
else
    echo "Some tests failed!"
    echo ""
    echo "--- Formatter output excerpt ---"
    echo "$OUTPUT" | grep -E "^\([a-z_\[\]\.]+\)" | head -20
    exit 1
fi
