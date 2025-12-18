//===-- zdb shim_full.cpp - Full internal API implementation --------------===//
//
// Implements Zig type formatters using LLDB's internal C++ API.
// Requires linking against both liblldb and libLLVM.
//
// Build with:
//   clang++ -std=c++17 -shared -fPIC \
//     -I/opt/homebrew/opt/llvm/include \
//     -L/opt/homebrew/opt/llvm/lib \
//     -llldb -lLLVM \
//     -Wl,-rpath,/opt/homebrew/opt/llvm/lib \
//     -o libzdb_full.dylib shim_full.cpp
//
//===----------------------------------------------------------------------===//

#include "lldb/API/LLDB.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/DataFormatters/TypeCategory.h"
#include "lldb/DataFormatters/TypeSynthetic.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/Utility/Stream.h"
#include "lldb/Target/Process.h"

#include <cstdio>

using namespace lldb;
using namespace lldb_private;

namespace lldb {
    bool PluginInitialize(SBDebugger debugger);
}

//===----------------------------------------------------------------------===//
// Zig Type Summary Providers
//===----------------------------------------------------------------------===//

// Zig slice: struct { ptr: [*]T, len: usize }
static bool ZigSliceSummary(ValueObject &valobj, Stream &stream,
                            const TypeSummaryOptions &options) {
    ValueObjectSP len_sp = valobj.GetChildMemberWithName("len");
    ValueObjectSP ptr_sp = valobj.GetChildMemberWithName("ptr");

    if (!len_sp || !ptr_sp) {
        stream.PutCString("(invalid slice)");
        return true;
    }

    uint64_t len = len_sp->GetValueAsUnsigned(0);
    uint64_t ptr = ptr_sp->GetValueAsUnsigned(0);

    stream.Printf("len=%llu ptr=0x%llx", (unsigned long long)len,
                  (unsigned long long)ptr);
    return true;
}

// Zig string slice: []const u8 or []u8
static bool ZigStringSummary(ValueObject &valobj, Stream &stream,
                             const TypeSummaryOptions &options) {
    ValueObjectSP len_sp = valobj.GetChildMemberWithName("len");
    ValueObjectSP ptr_sp = valobj.GetChildMemberWithName("ptr");

    if (!len_sp || !ptr_sp) {
        stream.PutCString("(invalid string)");
        return true;
    }

    uint64_t len = len_sp->GetValueAsUnsigned(0);
    uint64_t ptr = ptr_sp->GetValueAsUnsigned(0);

    // Read string content (limit to reasonable size)
    if (len > 0 && len < 1024 && ptr != 0) {
        ProcessSP process = valobj.GetProcessSP();
        if (process) {
            std::vector<char> buffer(len + 1, 0);
            Status error;
            size_t bytes_read = process->ReadMemory(ptr, buffer.data(), len, error);
            if (bytes_read == len && error.Success()) {
                stream.PutChar('"');
                // Escape special characters
                for (size_t i = 0; i < len; i++) {
                    char c = buffer[i];
                    if (c == '"') stream.PutCString("\\\"");
                    else if (c == '\\') stream.PutCString("\\\\");
                    else if (c == '\n') stream.PutCString("\\n");
                    else if (c == '\r') stream.PutCString("\\r");
                    else if (c == '\t') stream.PutCString("\\t");
                    else if (c >= 32 && c < 127) stream.PutChar(c);
                    else stream.Printf("\\x%02x", (unsigned char)c);
                }
                stream.PutChar('"');
                return true;
            }
        }
    }

    // Fallback to pointer display
    stream.Printf("len=%llu ptr=0x%llx", (unsigned long long)len,
                  (unsigned long long)ptr);
    return true;
}

// Zig optional: ?T
static bool ZigOptionalSummary(ValueObject &valobj, Stream &stream,
                               const TypeSummaryOptions &options) {
    // Zig optionals have 'some' (value) and possibly null marker
    ValueObjectSP some_sp = valobj.GetChildMemberWithName("some");

    // Check for null - Zig uses a sentinel pattern
    // For pointer types, check if the pointer is null
    // For other types, check child count or specific patterns

    if (!some_sp) {
        stream.PutCString("null");
        return true;
    }

    // Try to get the value
    uint64_t num_children = valobj.GetNumChildrenIgnoringErrors();
    if (num_children == 0) {
        stream.PutCString("null");
        return true;
    }

    // Show the wrapped value
    const char *summary = some_sp->GetSummaryAsCString();
    if (summary && summary[0]) {
        stream.PutCString(summary);
    } else {
        // For simple types, show the value
        uint64_t val = some_sp->GetValueAsUnsigned(0);
        stream.Printf("%llu", (unsigned long long)val);
    }
    return true;
}

// Zig error union: E!T
static bool ZigErrorUnionSummary(ValueObject &valobj, Stream &stream,
                                 const TypeSummaryOptions &options) {
    // Error unions have a tag and payload
    ValueObjectSP tag_sp = valobj.GetChildAtIndex(0);
    ValueObjectSP payload_sp = valobj.GetChildAtIndex(1);

    if (!tag_sp) {
        stream.PutCString("(invalid error union)");
        return true;
    }

    // The tag indicates error vs success
    uint64_t tag = tag_sp->GetValueAsUnsigned(0);

    if (tag != 0) {
        // It's an error - try to get error name
        const char *error_name = tag_sp->GetValueAsCString();
        if (error_name && error_name[0]) {
            stream.Printf("error.%s", error_name);
        } else {
            stream.Printf("error(%llu)", (unsigned long long)tag);
        }
    } else if (payload_sp) {
        // It's a value
        const char *summary = payload_sp->GetSummaryAsCString();
        if (summary && summary[0]) {
            stream.PutCString(summary);
        } else {
            uint64_t val = payload_sp->GetValueAsUnsigned(0);
            stream.Printf("%llu", (unsigned long long)val);
        }
    } else {
        stream.PutCString("(ok)");
    }
    return true;
}

// Zig tagged union: union(enum)
static bool ZigTaggedUnionSummary(ValueObject &valobj, Stream &stream,
                                  const TypeSummaryOptions &options) {
    // Tagged unions have a tag field
    ValueObjectSP tag_sp = valobj.GetChildMemberWithName("tag");
    if (!tag_sp) {
        tag_sp = valobj.GetChildAtIndex(0);
    }

    if (!tag_sp) {
        stream.PutCString("(invalid union)");
        return true;
    }

    const char *tag_name = tag_sp->GetValueAsCString();
    if (tag_name && tag_name[0]) {
        stream.Printf(".%s", tag_name);

        // Try to show the payload
        ValueObjectSP payload_sp = valobj.GetChildAtIndex(1);
        if (payload_sp) {
            const char *summary = payload_sp->GetSummaryAsCString();
            if (summary && summary[0]) {
                stream.Printf(" = %s", summary);
            }
        }
    } else {
        uint64_t tag = tag_sp->GetValueAsUnsigned(0);
        stream.Printf(".(%llu)", (unsigned long long)tag);
    }
    return true;
}

// Zig ArrayListUnmanaged
static bool ZigArrayListSummary(ValueObject &valobj, Stream &stream,
                                const TypeSummaryOptions &options) {
    ValueObjectSP items_sp = valobj.GetChildMemberWithName("items");
    ValueObjectSP capacity_sp = valobj.GetChildMemberWithName("capacity");

    if (!items_sp) {
        stream.PutCString("(invalid ArrayList)");
        return true;
    }

    // items is a slice, get its len
    ValueObjectSP len_sp = items_sp->GetChildMemberWithName("len");

    uint64_t len = len_sp ? len_sp->GetValueAsUnsigned(0) : 0;
    uint64_t cap = capacity_sp ? capacity_sp->GetValueAsUnsigned(0) : 0;

    stream.Printf("len=%llu capacity=%llu", (unsigned long long)len,
                  (unsigned long long)cap);
    return true;
}

//===----------------------------------------------------------------------===//
// Formatter Registration
//===----------------------------------------------------------------------===//

static void RegisterZigFormatters() {
    fprintf(stderr, "[zdb] Registering Zig formatters via internal API...\n");

    // Get or create the "zig" category
    TypeCategoryImplSP zig_category;
    DataVisualization::Categories::GetCategory(ConstString("zig"), zig_category, true);

    if (!zig_category) {
        fprintf(stderr, "[zdb] Failed to create 'zig' category\n");
        return;
    }

    // Set up default flags for our formatters
    TypeSummaryImpl::Flags flags;
    flags.SetCascades(true);
    flags.SetSkipPointers(false);
    flags.SetSkipReferences(false);
    flags.SetDontShowChildren(false);
    flags.SetDontShowValue(false);
    flags.SetShowMembersOneLiner(false);
    flags.SetHideItemNames(false);

    // Register slice formatters
    // Generic slice: []T
    auto slice_summary = std::make_shared<CXXFunctionSummaryFormat>(
        flags, ZigSliceSummary, "Zig slice formatter");
    zig_category->AddTypeSummary("^\\[\\].*$", eFormatterMatchRegex, slice_summary);

    // String slices: []u8, []const u8
    auto string_summary = std::make_shared<CXXFunctionSummaryFormat>(
        flags, ZigStringSummary, "Zig string formatter");
    zig_category->AddTypeSummary("^\\[\\]u8$", eFormatterMatchRegex, string_summary);
    zig_category->AddTypeSummary("^\\[\\]const u8$", eFormatterMatchRegex, string_summary);

    // Optional: ?T
    auto optional_summary = std::make_shared<CXXFunctionSummaryFormat>(
        flags, ZigOptionalSummary, "Zig optional formatter");
    zig_category->AddTypeSummary("^\\?.*$", eFormatterMatchRegex, optional_summary);

    // Error union: E!T
    auto error_union_summary = std::make_shared<CXXFunctionSummaryFormat>(
        flags, ZigErrorUnionSummary, "Zig error union formatter");
    zig_category->AddTypeSummary("^.*!.*$", eFormatterMatchRegex, error_union_summary);

    // Tagged union: union(enum)
    auto tagged_union_summary = std::make_shared<CXXFunctionSummaryFormat>(
        flags, ZigTaggedUnionSummary, "Zig tagged union formatter");
    zig_category->AddTypeSummary("^union\\(.*\\)$", eFormatterMatchRegex, tagged_union_summary);

    // ArrayListUnmanaged
    auto arraylist_summary = std::make_shared<CXXFunctionSummaryFormat>(
        flags, ZigArrayListSummary, "Zig ArrayList formatter");
    zig_category->AddTypeSummary("^array_list\\..*$", eFormatterMatchRegex, arraylist_summary);

    // Enable the category
    DataVisualization::Categories::Enable(ConstString("zig"));

    fprintf(stderr, "[zdb] Registered 6 summary formatters in 'zig' category\n");
}

//===----------------------------------------------------------------------===//
// Plugin Entry Point
//===----------------------------------------------------------------------===//

bool lldb::PluginInitialize(SBDebugger debugger) {
    fprintf(stderr, "[zdb] Zig LLDB plugin loaded (full internal API)\n");

    RegisterZigFormatters();

    fprintf(stderr, "[zdb] Ready - Zig types will be formatted automatically\n");
    return true;
}
