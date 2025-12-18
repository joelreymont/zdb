#!/usr/bin/env python3
"""
Dump LLDB internal symbol offsets for address hacking.

Usage:
    ./dump_offsets.py /path/to/liblldb.dylib > offsets_21.1.7.json

The output can be loaded by the C++ plugin to call internal functions.
"""

import subprocess
import sys
import json
import re

# Reference symbol (exported, stable across versions)
REFERENCE_SYMBOL = "_ZN4lldb10SBDebugger10InitializeEv"  # SBDebugger::Initialize

# Internal symbols we need
INTERNAL_SYMBOLS = [
    # DataVisualization
    ("_ZN12lldb_private17DataVisualization10Categories11GetCategoryENS_11ConstStringERNSt3__110shared_ptrINS_16TypeCategoryImplEEEb", "DataVisualization::Categories::GetCategory"),
    ("_ZN12lldb_private17DataVisualization10Categories6EnableERKNSt3__110shared_ptrINS_16TypeCategoryImplEEEj", "DataVisualization::Categories::Enable"),

    # TypeCategoryImpl
    ("_ZN12lldb_private16TypeCategoryImpl14AddTypeSummaryEN4llvm9StringRefEN4lldb18FormatterMatchTypeENSt3__110shared_ptrINS_15TypeSummaryImplEEE", "TypeCategoryImpl::AddTypeSummary"),
    ("_ZN12lldb_private16TypeCategoryImpl16AddTypeSyntheticEN4llvm9StringRefEN4lldb18FormatterMatchTypeENSt3__110shared_ptrINS_17SyntheticChildrenEEE", "TypeCategoryImpl::AddTypeSynthetic"),
    ("_ZN12lldb_private16TypeCategoryImpl13AddTypeFormatEN4llvm9StringRefEN4lldb18FormatterMatchTypeENSt3__110shared_ptrINS_14TypeFormatImplEEE", "TypeCategoryImpl::AddTypeFormat"),
    ("_ZN12lldb_private16TypeCategoryImpl13AddTypeFilterEN4llvm9StringRefEN4lldb18FormatterMatchTypeENSt3__110shared_ptrINS_14TypeFilterImplEEE", "TypeCategoryImpl::AddTypeFilter"),

    # CXXFunctionSummaryFormat (for C++ callbacks)
    ("_ZN12lldb_private24CXXFunctionSummaryFormatC2ERKNS_15TypeSummaryImpl5FlagsENSt3__18functionIFbRNS_11ValueObjectERNS_6StreamERKNS_18TypeSummaryOptionsEEEEPKcj", "CXXFunctionSummaryFormat::ctor"),

    # FormatManager (alternative path)
    ("_ZN12lldb_private13FormatManager11GetCategoryENS_11ConstStringEb", "FormatManager::GetCategory"),

    # AddCXXSynthetic - helper for registering synthetic children
    ("_ZN12lldb_private10formatters15AddCXXSyntheticENSt3__110shared_ptrINS_16TypeCategoryImplEEENS1_8functionIFPNS_25SyntheticChildrenFrontEndEPNS_20CXXSyntheticChildrenENS2_INS_11ValueObjectEEEEEEPKcN4llvm9StringRefENS_17SyntheticChildren5FlagsEb", "formatters::AddCXXSynthetic"),
]


def get_lldb_version(liblldb_path):
    """Extract version from library path or lldb --version."""
    # Try to get version from lldb binary in same directory
    import os
    lldb_dir = os.path.dirname(liblldb_path)
    lldb_bin = os.path.join(lldb_dir, "..", "bin", "lldb")

    if os.path.exists(lldb_bin):
        result = subprocess.run([lldb_bin, "--version"], capture_output=True, text=True)
        match = re.search(r"version (\d+\.\d+\.\d+)", result.stdout)
        if match:
            return match.group(1)

    # Fallback: extract from library name
    match = re.search(r"liblldb\.(\d+\.\d+\.\d+)\.dylib", liblldb_path)
    if match:
        return match.group(1)

    return "unknown"


def dump_offsets(liblldb_path):
    """Dump symbol offsets from liblldb."""
    # Get all symbols
    result = subprocess.run(
        ["nm", liblldb_path],
        capture_output=True,
        text=True
    )

    if result.returncode != 0:
        print(f"nm failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    # Parse nm output
    symbols = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            addr, typ, name = parts[0], parts[1], parts[2]
            # Remove leading underscore on macOS
            if name.startswith("_"):
                name = name[1:]
            symbols[name] = int(addr, 16)

    # Find reference symbol
    if REFERENCE_SYMBOL not in symbols:
        print(f"Reference symbol {REFERENCE_SYMBOL} not found!", file=sys.stderr)
        sys.exit(1)

    ref_offset = symbols[REFERENCE_SYMBOL]

    # Build offset table
    offsets = {
        "version": get_lldb_version(liblldb_path),
        "reference_symbol": REFERENCE_SYMBOL,
        "reference_offset": hex(ref_offset),
        "symbols": {}
    }

    for mangled, demangled in INTERNAL_SYMBOLS:
        if mangled in symbols:
            # Store as offset relative to reference (for verification)
            # and absolute offset (for direct use)
            offsets["symbols"][demangled] = {
                "mangled": mangled,
                "offset": hex(symbols[mangled]),
                "relative": hex(symbols[mangled] - ref_offset),
            }
        else:
            offsets["symbols"][demangled] = None
            print(f"Warning: {demangled} not found", file=sys.stderr)

    return offsets


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/liblldb.dylib", file=sys.stderr)
        sys.exit(1)

    liblldb_path = sys.argv[1]
    offsets = dump_offsets(liblldb_path)
    print(json.dumps(offsets, indent=2))


if __name__ == "__main__":
    main()
