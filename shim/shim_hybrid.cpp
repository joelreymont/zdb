//===-- zdb shim_hybrid.cpp - Hybrid SB + internal API --------------------===//
//
// Uses public SB API for category management, internal API for formatters.
// This avoids needing to create ConstString objects directly.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/LLDB.h"
#include "offset_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>

using namespace lldb;

namespace lldb {
    bool PluginInitialize(SBDebugger debugger);
}

//===----------------------------------------------------------------------===//
// ABI-compatible type definitions
// These must match LLDB's internal layout exactly
//===----------------------------------------------------------------------===//

// llvm::StringRef: { const char* Data, size_t Length }
struct StringRef {
    const char* Data;
    size_t Length;

    StringRef(const char* s) : Data(s), Length(strlen(s)) {}
    StringRef(const char* s, size_t len) : Data(s), Length(len) {}
};

// Use lldb::FormatterMatchType from the header

// TypeSummaryImpl::Flags - just a uint32_t
struct TypeSummaryFlags {
    uint32_t m_flags;

    TypeSummaryFlags() : m_flags(0x01) {} // eTypeOptionCascade = 1
    void SetCascades(bool v) { if (v) m_flags |= 0x01; else m_flags &= ~0x01; }
};

// Forward declarations for internal types
namespace lldb_private {
    class ValueObject;
    class Stream;
    class TypeSummaryOptions;
    class TypeCategoryImpl;
    class TypeSummaryImpl;
    class CXXFunctionSummaryFormat;
}

// Callback type for CXXFunctionSummaryFormat
using SummaryCallback = std::function<bool(lldb_private::ValueObject&,
                                           lldb_private::Stream&,
                                           const lldb_private::TypeSummaryOptions&)>;

//===----------------------------------------------------------------------===//
// Function pointer types for internal API
//===----------------------------------------------------------------------===//

// TypeCategoryImpl::AddTypeSummary(StringRef, FormatterMatchType, shared_ptr<TypeSummaryImpl>)
// This is a member function, so first arg is 'this' pointer
using AddTypeSummaryFn = void (*)(
    void* this_ptr,                              // TypeCategoryImpl*
    StringRef name,                              // type pattern
    lldb::FormatterMatchType match_type,
    std::shared_ptr<lldb_private::TypeSummaryImpl> summary_sp
);

// CXXFunctionSummaryFormat constructor
// CXXFunctionSummaryFormat(Flags&, Callback, const char*, uint32_t)
using CXXFunctionSummaryFormatCtorFn = void (*)(
    void* this_ptr,                              // placement new target
    const TypeSummaryFlags& flags,
    SummaryCallback callback,
    const char* description,
    uint32_t ptr_match_depth
);

//===----------------------------------------------------------------------===//
// Summary callback implementations using SB API
// These use SBValue which is the public wrapper around ValueObject
//===----------------------------------------------------------------------===//

// We can't easily use internal ValueObject from here, so we'll use a simpler
// approach: register callbacks that work through the public API

// Actually, we need to work with the raw ValueObject since that's what the
// callback receives. Let me use a minimal approach.

//===----------------------------------------------------------------------===//
// Minimal summary implementations
// These work with raw memory access, matching LLDB's internal ABI
//===----------------------------------------------------------------------===//

// ValueObject has these methods we need (found via nm):
// GetChildMemberWithName(StringRef, bool) -> shared_ptr<ValueObject>
// GetValueAsUnsigned(uint64_t, bool*) -> uint64_t

// For simplicity, let's try a really minimal test first

static bool TestCallback(void* valobj, void* stream, const void* options) {
    // Just output a test string
    // Stream::Printf is at some offset, but let's try PutCString
    // Actually we don't know the Stream API offsets

    // For now, just return true to indicate we handled it
    return true;
}

//===----------------------------------------------------------------------===//
// Plugin initialization
//===----------------------------------------------------------------------===//

static bool SetupFormatters(SBDebugger debugger) {
    fprintf(stderr, "[zdb] Setting up Zig formatters (hybrid mode)...\n");

    // Step 1: Create category via SB API
    SBTypeCategory category = debugger.CreateCategory("zig");
    if (!category.IsValid()) {
        fprintf(stderr, "[zdb] Failed to create 'zig' category\n");
        return false;
    }

    // Step 2: Get internal pointer via GetSP()
    // SBTypeCategory::GetSP() returns TypeCategoryImplSP
    // We need to call this method - it's public but we need its address

    // Check if GetSP is exported
    void* handle = dlopen("/opt/homebrew/opt/llvm/lib/liblldb.dylib", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[zdb] Failed to open liblldb\n");
        return false;
    }

    // Look for GetSP - it should be exported since it's public API
    void* get_sp = dlsym(handle, "_ZN4lldb14SBTypeCategory5GetSPEv");
    if (!get_sp) {
        fprintf(stderr, "[zdb] GetSP not found: %s\n", dlerror());
        // Try alternative mangled name
        get_sp = dlsym(handle, "_ZNK4lldb14SBTypeCategory5GetSPEv"); // const version
    }

    if (get_sp) {
        fprintf(stderr, "[zdb] Found GetSP at %p\n", get_sp);

        // GetSP returns shared_ptr<TypeCategoryImpl>
        // Call it: shared_ptr<TypeCategoryImpl> (SBTypeCategory::*GetSP)()
        using GetSPFn = std::shared_ptr<lldb_private::TypeCategoryImpl> (*)(SBTypeCategory*);
        auto get_sp_fn = (GetSPFn)get_sp;

        // This might crash if ABI doesn't match, but let's try
        auto category_impl = get_sp_fn(&category);

        if (category_impl) {
            fprintf(stderr, "[zdb] Got TypeCategoryImpl at %p\n", category_impl.get());

            // Now we can use the offset table to call AddTypeSummary
            // But we still need to create CXXFunctionSummaryFormat objects

            // For now, let's just verify we got the internal pointer
            fprintf(stderr, "[zdb] Internal category access successful!\n");
        } else {
            fprintf(stderr, "[zdb] GetSP returned null\n");
        }
    } else {
        fprintf(stderr, "[zdb] GetSP not exported, trying offset approach...\n");

        // The SBTypeCategory object contains m_opaque_sp as its only member
        // So &category + 0 should be the shared_ptr

        // shared_ptr layout: { T* ptr, control_block* ctrl }
        void** category_data = (void**)&category;
        void* impl_ptr = category_data[0];

        if (impl_ptr) {
            fprintf(stderr, "[zdb] TypeCategoryImpl at %p (from m_opaque_sp)\n", impl_ptr);
        }
    }

    // Step 3: Enable category via SB API
    category.SetEnabled(true);
    category.AddLanguage(eLanguageTypeC_plus_plus); // Zig uses C++ debug info

    fprintf(stderr, "[zdb] Category 'zig' created and enabled\n");

    // For now, we'll still recommend Python formatters until we can
    // successfully create CXXFunctionSummaryFormat objects
    fprintf(stderr, "[zdb] Note: Full formatter registration pending\n");
    fprintf(stderr, "[zdb] Use: command script import /path/to/zig_formatters.py\n");

    return true;
}

bool lldb::PluginInitialize(SBDebugger debugger) {
    fprintf(stderr, "[zdb] Zig LLDB plugin loaded (hybrid mode)\n");

    // Try to load internal symbols
    const char* version_str = SBDebugger::GetVersionString();
    const char* ver = strstr(version_str, "version ");
    if (ver) {
        ver += 8;
        char version[32];
        int i = 0;
        while (ver[i] && (isdigit(ver[i]) || ver[i] == '.') && i < 31) {
            version[i] = ver[i];
            i++;
        }
        version[i] = '\0';

        zdb::g_symbols.load("/opt/homebrew/opt/llvm/lib/liblldb.dylib", version);
    }

    SetupFormatters(debugger);

    return true;
}
