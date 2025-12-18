#!/bin/bash
# Automated tests for zdb formatters
set -e

cd "$(dirname "$0")/.."

# Use Homebrew LLVM (offsets are version-specific)
LLDB="${LLDB:-/opt/homebrew/opt/llvm/bin/lldb}"
if [ ! -x "$LLDB" ]; then
    echo "ERROR: LLDB not found at $LLDB"
    exit 1
fi

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

# Capture LLDB output - use frame variable to show all locals
OUTPUT=$("$LLDB" test/test_types \
    -o "plugin load zig-out/lib/libzdb.dylib" \
    -o "b test_types.zig:157" \
    -o "run" \
    -o "frame variable" \
    -o "p int_slice.ptr[0]" \
    -o "p int_slice.len" \
    -o "p string_slice" \
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

# Check formatters loaded (19 now)
check "Plugin loaded" "\[zdb\] Loaded [0-9]+ Zig formatters"

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

# Test expression evaluation
check "Expr: slice element" '\(int\) 1'
check "Expr: slice length" '\(unsigned long\) 5'
check "Expr: string slice" '"Hello, zdb debugger!"'

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
