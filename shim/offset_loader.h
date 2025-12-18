// offset_loader.h - Load internal LLDB symbols via offset tables
//
// Loads offsets from JSON files at runtime, allowing support for new
// LLDB versions without recompiling.
//
// Environment variables:
//   ZDB_OFFSETS_FILE  - Path to specific JSON file (highest priority)
//   ZDB_OFFSETS_DIR   - Directory containing lldb-X.Y.Z.json files
//
// Default search paths:
//   1. $ZDB_OFFSETS_FILE (if set)
//   2. $ZDB_OFFSETS_DIR/lldb-X.Y.Z.json (if set)
//   3. ~/.config/zdb/offsets/lldb-X.Y.Z.json
//   4. /usr/local/share/zdb/offsets/lldb-X.Y.Z.json
//   5. <plugin_dir>/../offsets/lldb-X.Y.Z.json
//
// Generate offset files with:
//   python3 tools/dump_offsets.py /path/to/liblldb.dylib > lldb-X.Y.Z.json

#pragma once

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <string>

namespace zdb {

struct OffsetTable {
    std::string version;
    std::string reference_symbol;
    uintptr_t reference_offset = 0;

    // Symbol offsets (0 = not available)
    uintptr_t GetCategory = 0;
    uintptr_t Enable = 0;
    uintptr_t AddTypeSummary = 0;
    uintptr_t AddTypeSynthetic = 0;
    uintptr_t AddTypeFormat = 0;
    uintptr_t AddTypeFilter = 0;
    uintptr_t CXXFunctionSummaryFormat_ctor = 0;
    uintptr_t FormatManager_GetCategory = 0;
};

// Simple JSON value extraction (no external dependencies)
static uintptr_t extract_hex(const std::string& json, const std::string& key) {
    // Find "key": "0x..."
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;

    pos += search.length();
    // Skip whitespace
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    // Check for null
    if (json.substr(pos, 4) == "null") return 0;

    // Find hex value in quotes
    if (json[pos] == '"') {
        pos++;
        size_t end = json.find('"', pos);
        if (end != std::string::npos) {
            std::string hex = json.substr(pos, end - pos);
            return strtoull(hex.c_str(), nullptr, 16);
        }
    }
    return 0;
}

static std::string extract_string(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos += search.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (json[pos] == '"') {
        pos++;
        size_t end = json.find('"', pos);
        if (end != std::string::npos) {
            return json.substr(pos, end - pos);
        }
    }
    return "";
}

static uintptr_t extract_symbol_offset(const std::string& json, const std::string& symbol_name) {
    // Find the symbol block: "Symbol Name": { ... "offset": "0x..." ... }
    std::string search = "\"" + symbol_name + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;

    // Find the block
    size_t brace = json.find('{', pos);
    if (brace == std::string::npos) return 0;

    size_t end_brace = json.find('}', brace);
    if (end_brace == std::string::npos) return 0;

    std::string block = json.substr(brace, end_brace - brace + 1);
    return extract_hex(block, "offset");
}

class InternalSymbols {
public:
    bool loaded = false;
    uintptr_t base = 0;
    OffsetTable table;
    std::string json_path;

    // Resolved function pointers (set after load)
    void* GetCategory = nullptr;
    void* Enable = nullptr;
    void* AddTypeSummary = nullptr;
    void* AddTypeSynthetic = nullptr;
    void* AddTypeFormat = nullptr;
    void* AddTypeFilter = nullptr;
    void* CXXFunctionSummaryFormat_ctor = nullptr;

    bool load_json(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string json = buffer.str();

        // Parse JSON
        table.version = extract_string(json, "version");
        table.reference_symbol = extract_string(json, "reference_symbol");
        table.reference_offset = extract_hex(json, "reference_offset");

        // Extract symbol offsets
        table.GetCategory = extract_symbol_offset(json, "DataVisualization::Categories::GetCategory");
        table.Enable = extract_symbol_offset(json, "DataVisualization::Categories::Enable");
        table.AddTypeSummary = extract_symbol_offset(json, "TypeCategoryImpl::AddTypeSummary");
        table.AddTypeSynthetic = extract_symbol_offset(json, "TypeCategoryImpl::AddTypeSynthetic");
        table.AddTypeFormat = extract_symbol_offset(json, "TypeCategoryImpl::AddTypeFormat");
        table.AddTypeFilter = extract_symbol_offset(json, "TypeCategoryImpl::AddTypeFilter");
        table.CXXFunctionSummaryFormat_ctor = extract_symbol_offset(json, "CXXFunctionSummaryFormat::ctor");
        table.FormatManager_GetCategory = extract_symbol_offset(json, "FormatManager::GetCategory");

        json_path = path;
        return table.reference_offset != 0;
    }

    std::string find_offsets_file(const char* version) {
        std::string filename = std::string("lldb-") + version + ".json";

        // 1. Check ZDB_OFFSETS_FILE env var
        const char* explicit_file = getenv("ZDB_OFFSETS_FILE");
        if (explicit_file && explicit_file[0]) {
            return explicit_file;
        }

        // 2. Check ZDB_OFFSETS_DIR env var
        const char* offsets_dir = getenv("ZDB_OFFSETS_DIR");
        if (offsets_dir && offsets_dir[0]) {
            std::string path = std::string(offsets_dir) + "/" + filename;
            std::ifstream test(path);
            if (test.good()) return path;
        }

        // 3. Check ~/.config/zdb/offsets/
        const char* home = getenv("HOME");
        if (home) {
            std::string path = std::string(home) + "/.config/zdb/offsets/" + filename;
            std::ifstream test(path);
            if (test.good()) return path;
        }

        // 4. Check /usr/local/share/zdb/offsets/
        {
            std::string path = "/usr/local/share/zdb/offsets/" + filename;
            std::ifstream test(path);
            if (test.good()) return path;
        }

        // 5. Check relative to plugin location (for development)
        // This would require getting the plugin's path, which is complex
        // For now, skip this

        return "";
    }

    bool load(const char* liblldb_path, const char* version) {
        // Find and load offset JSON
        std::string json_file = find_offsets_file(version);

        if (json_file.empty()) {
            fprintf(stderr, "[zdb] No offset file found for LLDB %s\n", version);
            fprintf(stderr, "[zdb] Generate one with: python3 tools/dump_offsets.py %s > lldb-%s.json\n",
                    liblldb_path, version);
            fprintf(stderr, "[zdb] Then set ZDB_OFFSETS_FILE or ZDB_OFFSETS_DIR environment variable\n");
            return false;
        }

        if (!load_json(json_file)) {
            fprintf(stderr, "[zdb] Failed to parse: %s\n", json_file.c_str());
            return false;
        }

        // Verify version matches
        if (table.version != version) {
            fprintf(stderr, "[zdb] Warning: offset file version (%s) doesn't match LLDB (%s)\n",
                    table.version.c_str(), version);
        }

        // Open library
        void* handle = dlopen(liblldb_path, RTLD_NOW);
        if (!handle) {
            fprintf(stderr, "[zdb] dlopen failed: %s\n", dlerror());
            return false;
        }

        // Find reference symbol to calculate base
        std::string ref_sym = table.reference_symbol.empty()
            ? "_ZN4lldb10SBDebugger10InitializeEv"
            : table.reference_symbol;

        void* ref = dlsym(handle, ref_sym.c_str());
        if (!ref) {
            fprintf(stderr, "[zdb] Reference symbol not found: %s\n", ref_sym.c_str());
            dlclose(handle);
            return false;
        }

        // Calculate base address
        base = (uintptr_t)ref - table.reference_offset;

        // Resolve all symbols
        if (table.GetCategory) GetCategory = (void*)(base + table.GetCategory);
        if (table.Enable) Enable = (void*)(base + table.Enable);
        if (table.AddTypeSummary) AddTypeSummary = (void*)(base + table.AddTypeSummary);
        if (table.AddTypeSynthetic) AddTypeSynthetic = (void*)(base + table.AddTypeSynthetic);
        if (table.AddTypeFormat) AddTypeFormat = (void*)(base + table.AddTypeFormat);
        if (table.AddTypeFilter) AddTypeFilter = (void*)(base + table.AddTypeFilter);
        if (table.CXXFunctionSummaryFormat_ctor)
            CXXFunctionSummaryFormat_ctor = (void*)(base + table.CXXFunctionSummaryFormat_ctor);

        loaded = true;
        // Note: We don't dlclose because we need the library loaded
        return true;
    }
};

// Global instance
static InternalSymbols g_symbols;

} // namespace zdb
