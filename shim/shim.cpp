//===-- zdb shim.cpp - Zig LLDB Plugin with internal API hack -------------===//
//
// WARNING: This uses offset-based access to LLDB internal symbols.
// It WILL break when LLDB is updated. Use zig_formatters.py for stability.
//
// To enable: set ZDB_USE_INTERNAL_API=1 before loading
//
//===----------------------------------------------------------------------===//

#include "lldb/API/LLDB.h"
#include "offset_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace lldb;

namespace lldb {
    bool PluginInitialize(SBDebugger debugger);
}

// Verify offset table works by checking symbol resolution
static bool VerifyOffsets(SBDebugger debugger) {
    // Detect LLDB version
    const char* version_str = SBDebugger::GetVersionString();
    fprintf(stderr, "[zdb] LLDB version: %s\n", version_str);

    // Extract version number (e.g., "lldb version 21.1.7" -> "21.1.7")
    const char* ver = strstr(version_str, "version ");
    if (!ver) {
        fprintf(stderr, "[zdb] Cannot parse version\n");
        return false;
    }
    ver += 8; // Skip "version "

    char version[32];
    int i = 0;
    while (ver[i] && (isdigit(ver[i]) || ver[i] == '.') && i < 31) {
        version[i] = ver[i];
        i++;
    }
    version[i] = '\0';

    // Load symbols
    if (!zdb::g_symbols.load("/opt/homebrew/opt/llvm/lib/liblldb.dylib", version)) {
        return false;
    }

    fprintf(stderr, "[zdb] Offset table verified for version %s\n", version);
    fprintf(stderr, "[zdb] Resolved symbols:\n");
    fprintf(stderr, "[zdb]   DataVisualization::Categories::GetCategory: %p\n", zdb::g_symbols.GetCategory);
    fprintf(stderr, "[zdb]   DataVisualization::Categories::Enable:      %p\n", zdb::g_symbols.Enable);
    fprintf(stderr, "[zdb]   TypeCategoryImpl::AddTypeSummary:           %p\n", zdb::g_symbols.AddTypeSummary);
    fprintf(stderr, "[zdb]   TypeCategoryImpl::AddTypeSynthetic:         %p\n", zdb::g_symbols.AddTypeSynthetic);
    fprintf(stderr, "[zdb]   CXXFunctionSummaryFormat::ctor:             %p\n", zdb::g_symbols.CXXFunctionSummaryFormat_ctor);

    fprintf(stderr, "\n[zdb] To actually USE these addresses, we would need to:\n");
    fprintf(stderr, "[zdb]   1. Create lldb_private::ConstString for category name\n");
    fprintf(stderr, "[zdb]   2. Create shared_ptr<TypeCategoryImpl> via GetCategory\n");
    fprintf(stderr, "[zdb]   3. Create CXXFunctionSummaryFormat with our callback\n");
    fprintf(stderr, "[zdb]   4. Call AddTypeSummary with the formatter\n");
    fprintf(stderr, "[zdb]\n");
    fprintf(stderr, "[zdb] This requires matching the exact C++ ABI (vtable layouts,\n");
    fprintf(stderr, "[zdb] shared_ptr internals, ConstString representation).\n");
    fprintf(stderr, "[zdb]\n");
    fprintf(stderr, "[zdb] Recommendation: Use zig_formatters.py for stable formatters.\n");

    return true;
}

bool lldb::PluginInitialize(SBDebugger debugger) {
    fprintf(stderr, "[zdb] Zig LLDB plugin loaded\n");

    // Check if user wants internal API verification (experimental)
    const char* use_internal = getenv("ZDB_USE_INTERNAL_API");
    if (use_internal && strcmp(use_internal, "1") == 0) {
        fprintf(stderr, "[zdb] Verifying internal API offsets (experimental)...\n");
        VerifyOffsets(debugger);
    }

    fprintf(stderr, "[zdb] For type formatters, run: command script import /path/to/zig_formatters.py\n");
    return true;
}
