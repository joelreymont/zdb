//===-- zdb shim_callback.cpp - Zig LLDB Plugin (Internal API) -----------===//
//
// Uses offset table to call LLDB internal APIs directly, bypassing the broken
// SBTypeSummary registration path.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/LLDB.h"
#include "offset_loader.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace lldb;

namespace lldb {
    bool PluginInitialize(SBDebugger debugger);
}

//===----------------------------------------------------------------------===//
// Keep formatters alive (prevent destruction)
//===----------------------------------------------------------------------===//

static std::vector<SBTypeSummary> g_formatters;

//===----------------------------------------------------------------------===//
// Formatter Callbacks
//===----------------------------------------------------------------------===//

static bool ZigSliceSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue len = value.GetChildMemberWithName("len");
    SBValue ptr = value.GetChildMemberWithName("ptr");
    if (len.IsValid() && ptr.IsValid()) {
        stream.Printf("len=%llu ptr=0x%llx",
            (unsigned long long)len.GetValueAsUnsigned(0),
            (unsigned long long)ptr.GetValueAsUnsigned(0));
        return true;
    }
    return false;
}

static bool ZigStringSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue len = value.GetChildMemberWithName("len");
    SBValue ptr = value.GetChildMemberWithName("ptr");
    if (!len.IsValid() || !ptr.IsValid()) return false;

    uint64_t len_val = len.GetValueAsUnsigned(0);
    uint64_t ptr_val = ptr.GetValueAsUnsigned(0);

    if (len_val > 0 && len_val < 1024 && ptr_val != 0) {
        SBProcess process = value.GetProcess();
        if (process.IsValid()) {
            std::vector<char> buffer(len_val + 1, 0);
            SBError error;
            size_t bytes_read = process.ReadMemory(ptr_val, buffer.data(), len_val, error);
            if (bytes_read == len_val && error.Success()) {
                stream.Printf("\"%s\"", buffer.data());
                return true;
            }
        }
    }
    stream.Printf("len=%llu ptr=0x%llx", (unsigned long long)len_val, (unsigned long long)ptr_val);
    return true;
}

static bool ZigOptionalSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue child = value.GetChildAtIndex(0);
    if (!child.IsValid()) {
        stream.Printf("null");
        return true;
    }
    const char* name = child.GetName();
    if (name && strcmp(name, "null") == 0) {
        stream.Printf("null");
        return true;
    }
    const char* summary = child.GetSummary();
    if (summary && summary[0]) {
        stream.Printf("%s", summary);
        return true;
    }
    const char* val = child.GetValue();
    if (val && val[0]) {
        stream.Printf("%s", val);
        return true;
    }
    stream.Printf("(has value)");
    return true;
}

static bool ZigErrorUnionSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue error = value.GetChildMemberWithName("error");
    if (!error.IsValid()) error = value.GetChildMemberWithName("err");
    if (!error.IsValid()) return false;

    uint64_t error_val = error.GetValueAsUnsigned(0);
    if (error_val != 0) {
        const char* error_name = error.GetValue();
        if (error_name) {
            stream.Printf("error.%s", error_name);
        } else {
            stream.Printf("error(%llu)", (unsigned long long)error_val);
        }
        return true;
    }
    SBValue payload = value.GetChildMemberWithName("value");
    if (payload.IsValid()) {
        const char* summary = payload.GetSummary();
        if (summary && summary[0]) {
            stream.Printf("%s", summary);
            return true;
        }
        const char* val = payload.GetValue();
        if (val && val[0]) {
            stream.Printf("%s", val);
            return true;
        }
    }
    stream.Printf("(success)");
    return true;
}

static bool ZigTaggedUnionSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue tag = value.GetChildMemberWithName("tag");
    if (!tag.IsValid()) return false;

    const char* tag_val = tag.GetValue();
    if (!tag_val) return false;

    stream.Printf(".%s", tag_val);

    // Try to show active payload
    SBValue payload = value.GetChildMemberWithName("payload");
    if (payload.IsValid()) {
        SBValue active = payload.GetChildMemberWithName(tag_val);
        if (active.IsValid()) {
            const char* summary = active.GetSummary();
            if (summary && summary[0]) {
                stream.Printf(" = %s", summary);
            }
        }
    }
    return true;
}

static bool ZigArrayListSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue items = value.GetChildMemberWithName("items");
    SBValue capacity = value.GetChildMemberWithName("capacity");

    if (items.IsValid()) {
        SBValue len = items.GetChildMemberWithName("len");
        if (len.IsValid()) {
            stream.Printf("len=%llu", (unsigned long long)len.GetValueAsUnsigned(0));
            if (capacity.IsValid()) {
                stream.Printf(" capacity=%llu", (unsigned long long)capacity.GetValueAsUnsigned(0));
            }
            return true;
        }
    }
    stream.Printf("(ArrayList)");
    return true;
}

static bool ZigHashMapSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue size = value.GetChildMemberWithName("size");
    if (!size.IsValid()) size = value.GetChildMemberWithName("count");
    if (size.IsValid()) {
        stream.Printf("size=%llu", (unsigned long long)size.GetValueAsUnsigned(0));
        return true;
    }
    stream.Printf("(HashMap)");
    return true;
}

static bool ZigBoundedArraySummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue len = value.GetChildMemberWithName("len");
    if (len.IsValid()) {
        stream.Printf("len=%llu", (unsigned long long)len.GetValueAsUnsigned(0));
        return true;
    }
    stream.Printf("(BoundedArray)");
    return true;
}

static bool ZigMultiArrayListSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue len = value.GetChildMemberWithName("len");
    SBValue capacity = value.GetChildMemberWithName("capacity");
    if (len.IsValid()) {
        stream.Printf("len=%llu", (unsigned long long)len.GetValueAsUnsigned(0));
        if (capacity.IsValid()) {
            stream.Printf(" capacity=%llu", (unsigned long long)capacity.GetValueAsUnsigned(0));
        }
        return true;
    }
    stream.Printf("(MultiArrayList)");
    return true;
}

static bool ZigSegmentedListSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    SBValue len = value.GetChildMemberWithName("len");
    if (len.IsValid()) {
        stream.Printf("len=%llu", (unsigned long long)len.GetValueAsUnsigned(0));
        return true;
    }
    stream.Printf("(SegmentedList)");
    return true;
}

static bool ZigCStringSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    uint64_t ptr_val = value.GetValueAsUnsigned(0);
    if (ptr_val == 0) {
        stream.Printf("null");
        return true;
    }
    SBProcess process = value.GetProcess();
    if (process.IsValid()) {
        char buffer[256];
        SBError error;
        size_t bytes_read = process.ReadCStringFromMemory(ptr_val, buffer, sizeof(buffer), error);
        if (bytes_read > 0 && error.Success()) {
            stream.Printf("\"%s\"", buffer);
            return true;
        }
    }
    stream.Printf("0x%llx", (unsigned long long)ptr_val);
    return true;
}

static bool ZigArraySummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    uint32_t num = value.GetNumChildren();
    stream.Printf("[%u]...", num);
    return true;
}

static bool ZigPointerSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    uint64_t ptr_val = value.GetValueAsUnsigned(0);
    if (ptr_val == 0) {
        stream.Printf("null");
        return true;
    }
    SBValue deref = value.Dereference();
    if (deref.IsValid()) {
        const char* summary = deref.GetSummary();
        if (summary && summary[0]) {
            stream.Printf("-> %s", summary);
            return true;
        }
        const char* val = deref.GetValue();
        if (val && val[0]) {
            stream.Printf("-> %s", val);
            return true;
        }
    }
    stream.Printf("0x%llx", (unsigned long long)ptr_val);
    return true;
}

static bool ZigStructSummary(SBValue value, SBTypeSummaryOptions options, SBStream &stream) {
    uint32_t num_children = value.GetNumChildren();

    // Enum: no children, has a value
    if (num_children == 0) {
        const char* val = value.GetValue();
        if (val && val[0]) {
            stream.Printf(".%s", val);
            return true;
        }
        stream.Printf("{}");
        return true;
    }

    // Small structs: show inline
    if (num_children <= 3) {
        stream.Printf("{ ");
        for (uint32_t i = 0; i < num_children; i++) {
            if (i > 0) stream.Printf(", ");
            SBValue child = value.GetChildAtIndex(i);
            if (child.IsValid()) {
                const char* name = child.GetName();
                const char* summary = child.GetSummary();
                if (summary && summary[0]) {
                    stream.Printf(".%s=%s", name ? name : "?", summary);
                } else {
                    const char* val = child.GetValue();
                    if (val && val[0]) {
                        stream.Printf(".%s=%s", name ? name : "?", val);
                    }
                }
            }
        }
        stream.Printf(" }");
        return true;
    }

    stream.Printf("{ %u fields }", num_children);
    return true;
}

//===----------------------------------------------------------------------===//
// ABI-Compatible Types for Internal API
//===----------------------------------------------------------------------===//

// llvm::StringRef: passed as { const char*, size_t } on ARM64
// On Apple ARM64, small structs (<=16 bytes) passed in registers

// shared_ptr layout: { T* ptr, control_block* ctrl }
struct SharedPtrLayout {
    void* ptr;
    void* ctrl;
};

// ConstString is just a const char* wrapper
struct ConstString {
    const char* m_string;
    ConstString(const char* s) : m_string(s) {}
};

//===----------------------------------------------------------------------===//
// Internal API Registration
//===----------------------------------------------------------------------===//

// Function pointer types matching LLDB internal ABI
// GetCategory: void(ConstString, shared_ptr<TypeCategoryImpl>&, bool)
// On ARM64: x0=ConstString (8 bytes), x1=&out_sp, x2=bool
using GetCategoryFn = void (*)(
    const char* name,           // ConstString passed as raw pointer
    SharedPtrLayout* out_sp,    // Output: shared_ptr<TypeCategoryImpl>
    bool can_create
);

// Enable: void(const shared_ptr<TypeCategoryImpl>&, uint32_t)
using EnableFn = void (*)(
    const SharedPtrLayout* category_sp,
    uint32_t position
);

// AddTypeSummary: member function
// void TypeCategoryImpl::AddTypeSummary(StringRef, FormatterMatchType, shared_ptr<TypeSummaryImpl>)
// On ARM64: non-trivial types passed indirectly (pointer to struct)
using AddTypeSummaryFn = void (*)(
    void* this_ptr,             // TypeCategoryImpl*
    const char* name_ptr,       // StringRef.data
    size_t name_len,            // StringRef.length
    int match_type,             // FormatterMatchType enum
    SharedPtrLayout* sp         // shared_ptr passed indirectly
);

static bool RegisterFormatter(
    void* category_impl,
    AddTypeSummaryFn add_fn,
    const char* pattern,
    bool (*callback)(SBValue, SBTypeSummaryOptions, SBStream&),
    const char* description,
    bool is_regex = true
) {
    SBTypeSummary summary = SBTypeSummary::CreateWithCallback(
        callback, eTypeOptionCascade, description);

    if (!summary.IsValid()) return false;

    g_formatters.push_back(summary);

    // Extract shared_ptr from SBTypeSummary (only contains m_opaque_sp)
    SharedPtrLayout* sp = (SharedPtrLayout*)&summary;

    // Call internal AddTypeSummary
    // FormatterMatchType: 0=Exact, 1=Regex, 2=Callback
    add_fn(
        category_impl,
        pattern, strlen(pattern),
        is_regex ? 1 : 0,
        sp  // ARM64: non-trivial types passed indirectly
    );

    return true;
}

static bool RegisterWithInternalAPI(SBDebugger debugger) {
    // Get LLDB version
    const char* version_str = SBDebugger::GetVersionString();
    const char* ver = strstr(version_str, "version ");
    if (!ver) {
        fprintf(stderr, "[zdb] Could not parse LLDB version\n");
        return false;
    }
    ver += 8;

    char version[32];
    int i = 0;
    while (ver[i] && (isdigit(ver[i]) || ver[i] == '.') && i < 31) {
        version[i] = ver[i];
        i++;
    }
    version[i] = '\0';

    // Load offsets
    if (!zdb::g_symbols.load("/opt/homebrew/opt/llvm/lib/liblldb.dylib", version)) {
        fprintf(stderr, "[zdb] No offsets for LLDB %s\n", version);
        return false;
    }

    // Get function pointers
    auto GetCategory = (GetCategoryFn)(zdb::g_symbols.base + zdb::g_symbols.table.GetCategory);
    auto Enable = (EnableFn)(zdb::g_symbols.base + zdb::g_symbols.table.Enable);
    auto AddTypeSummary = (AddTypeSummaryFn)(zdb::g_symbols.base + zdb::g_symbols.table.AddTypeSummary);

    if (!zdb::g_symbols.table.GetCategory || !zdb::g_symbols.table.AddTypeSummary) {
        fprintf(stderr, "[zdb] Missing required offsets\n");
        return false;
    }

    // Get/create "zig" category
    SharedPtrLayout category_sp = {nullptr, nullptr};
    GetCategory("zig", &category_sp, true);

    if (!category_sp.ptr) {
        fprintf(stderr, "[zdb] Failed to create 'zig' category\n");
        return false;
    }

    // Register formatters - LLDB uses LAST-MATCH, so register generic first, specific last

    // 1. Catch-all for structs/enums (lowest priority)
    // Matches: module.TypeName (e.g., test_types.Color, test_types.MyStruct)
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^[a-z_][a-z0-9_]*\\.[A-Z][A-Za-z0-9_]*$", ZigStructSummary, "Zig struct/enum", true);
    // Matches: standalone PascalCase types (e.g., Color, MyStruct)
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^[A-Z][A-Za-z0-9_]*$", ZigStructSummary, "Zig type", true);

    // 2. Generic Zig types
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[.*\\].*$", ZigArraySummary, "Zig array", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\].*$", ZigSliceSummary, "Zig slice", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\?.*$", ZigOptionalSummary, "Zig optional", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^.*!.*$", ZigErrorUnionSummary, "Zig error union", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^union\\(.*\\)$", ZigTaggedUnionSummary, "Zig tagged union", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\*.*$", ZigPointerSummary, "Zig pointer", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\*\\].*$", ZigPointerSummary, "Zig many pointer", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\*:.*\\].*$", ZigPointerSummary, "Zig sentinel pointer", true);

    // 3. std library types
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^array_list\\..*$", ZigArrayListSummary, "Zig ArrayList", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^hash_map\\..*$", ZigHashMapSummary, "Zig HashMap", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^bounded_array\\..*$", ZigBoundedArraySummary, "Zig BoundedArray", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^multi_array_list\\..*$", ZigMultiArrayListSummary, "Zig MultiArrayList", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^segmented_list\\..*$", ZigSegmentedListSummary, "Zig SegmentedList", true);

    // 4. C strings (sentinel-terminated u8 pointers)
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\*:0\\]u8$", ZigCStringSummary, "Zig C string", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\*:0\\]const u8$", ZigCStringSummary, "Zig const C string", true);

    // 5. Specific string types (highest priority)
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\]const u8$", ZigStringSummary, "Zig const string", true);
    RegisterFormatter(category_sp.ptr, AddTypeSummary, "^\\[\\]u8$", ZigStringSummary, "Zig string", true);

    // Enable category
    if (zdb::g_symbols.table.Enable) {
        Enable(&category_sp, 0);
    }

    return true;
}

//===----------------------------------------------------------------------===//
// Plugin Entry Point
//===----------------------------------------------------------------------===//

bool lldb::PluginInitialize(SBDebugger debugger) {
    if (RegisterWithInternalAPI(debugger)) {
        fprintf(stderr, "[zdb] Loaded %zu Zig formatters\n", g_formatters.size());
        return true;
    }

    fprintf(stderr, "[zdb] Failed to load. Use: command script import zig_formatters.py\n");
    return true;
}
