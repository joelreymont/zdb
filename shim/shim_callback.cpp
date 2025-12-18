//===-- zdb shim_callback.cpp - Zig LLDB Plugin (Internal API) -----------===//
//
// Uses offset table to call LLDB internal APIs directly, bypassing the broken
// SBTypeSummary registration path.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/LLDB.h"
#include "offset_loader.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <regex>
#include <functional>

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
    // Zig optionals have 'some' (discriminant) and 'data' (payload) fields
    // some == 1 means has value, some == 0 means null
    SBValue some = value.GetChildMemberWithName("some");
    if (some.IsValid()) {
        if (some.GetValueAsUnsigned(0) == 0) {
            stream.Printf("null");
            return true;
        }
        // Has value - show the data field
        SBValue data = value.GetChildMemberWithName("data");
        if (data.IsValid()) {
            const char* summary = data.GetSummary();
            if (summary && summary[0]) {
                stream.Printf("%s", summary);
                return true;
            }
            const char* val = data.GetValue();
            if (val && val[0]) {
                stream.Printf("%s", val);
                return true;
            }
        }
        stream.Printf("(has value)");
        return true;
    }

    // Fallback: try older layout with 'null' named child
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
    // Zig error unions have 'tag' (error code, 0 = success) and 'value' (payload) fields
    SBValue tag = value.GetChildMemberWithName("tag");
    if (!tag.IsValid()) {
        // Fallback: try older field names
        tag = value.GetChildMemberWithName("error");
        if (!tag.IsValid()) tag = value.GetChildMemberWithName("err");
    }
    if (!tag.IsValid()) return false;

    uint64_t tag_val = tag.GetValueAsUnsigned(0);
    if (tag_val != 0) {
        // Error case - show error name if available
        const char* error_name = tag.GetValue();
        if (error_name) {
            stream.Printf("error.%s", error_name);
        } else {
            stream.Printf("error(%llu)", (unsigned long long)tag_val);
        }
        return true;
    }

    // Success case - show payload
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

// shared_ptr layout: { T* ptr, control_block* ctrl }
struct SharedPtrLayout {
    void* ptr;
    void* ctrl;
};

//===----------------------------------------------------------------------===//
// ABI-Compatible Types for Internal API
//===----------------------------------------------------------------------===//

// llvm::StringRef: passed as { const char*, size_t } on ARM64
// On Apple ARM64, small structs (<=16 bytes) passed in registers

// Note: SharedPtrLayout defined earlier in synthetic section

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

// Resolve liblldb path from the running process using dladdr
static std::string ResolveLibLLDBPath() {
    // 1. Check environment variable
    const char* env_path = getenv("ZDB_LIBLLDB_PATH");
    if (env_path && env_path[0]) {
        return env_path;
    }

    // 2. Use dladdr on SBDebugger::GetVersionString to find loaded liblldb
    Dl_info info;
    if (dladdr((void*)&SBDebugger::GetVersionString, &info) && info.dli_fname) {
        return info.dli_fname;
    }

    // 3. Platform-specific fallbacks
#if defined(__APPLE__)
    // Try Homebrew ARM64 first, then x86_64
    const char* fallbacks[] = {
        "/opt/homebrew/opt/llvm/lib/liblldb.dylib",
        "/usr/local/opt/llvm/lib/liblldb.dylib",
        "/Library/Developer/CommandLineTools/usr/lib/liblldb.dylib",
        nullptr
    };
#else
    // Linux fallbacks
    const char* fallbacks[] = {
        "/usr/lib/llvm-18/lib/liblldb.so",
        "/usr/lib/llvm-17/lib/liblldb.so",
        "/usr/lib/liblldb.so",
        nullptr
    };
#endif
    for (const char** p = fallbacks; *p; ++p) {
        if (access(*p, R_OK) == 0) {
            return *p;
        }
    }

    return "";
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

    // Resolve liblldb path dynamically
    std::string liblldb_path = ResolveLibLLDBPath();
    if (liblldb_path.empty()) {
        fprintf(stderr, "[zdb] Could not find liblldb. Set ZDB_LIBLLDB_PATH env var.\n");
        return false;
    }

    // Load offsets
    if (!zdb::g_symbols.load(liblldb_path.c_str(), version)) {
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

    // 6. Synthetic children providers
    // AddCXXSynthetic crashes due to std::function ABI incompatibility
    // AddTypeSynthetic with fake shared_ptr doesn't register properly
    // For now, skip synthetic registration - expression transformation still works
    fprintf(stderr, "[zdb] Synthetic providers: skipped (ABI barrier)\n");
    // NOTE: The expression transformer in ZigExpressionCommand still handles
    // slice[n], arraylist[n], optional.?, and err catch val syntax

    // Enable category
    if (zdb::g_symbols.table.Enable) {
        Enable(&category_sp, 0);
    }

    return true;
}

//===----------------------------------------------------------------------===//
// Zig Expression Transformer
//===----------------------------------------------------------------------===//

// Transform Zig expressions to C equivalents:
//   slice[n]       -> slice.ptr[n]
//   arraylist[n]   -> arraylist.items.ptr[n]
//   optional.?     -> optional.data
//   err catch val  -> (err.tag == 0 ? err.value : val)

namespace {

// Navigate a dot-separated path from a frame to get the SBValue
SBValue GetValueAtPath(SBFrame frame, const std::string& path) {
    if (path.empty()) return SBValue();

    size_t dot = path.find('.');
    std::string root = (dot == std::string::npos) ? path : path.substr(0, dot);
    SBValue val = frame.FindVariable(root.c_str());

    if (!val.IsValid()) return SBValue();
    if (dot == std::string::npos) return val;

    // Navigate remaining path segments
    size_t pos = dot + 1;
    while (pos < path.length()) {
        size_t next = path.find('.', pos);
        std::string member = (next == std::string::npos)
            ? path.substr(pos)
            : path.substr(pos, next - pos);
        val = val.GetChildMemberWithName(member.c_str());
        if (!val.IsValid()) return SBValue();
        pos = (next == std::string::npos) ? path.length() : next + 1;
    }
    return val;
}

// Type detection helpers
bool IsZigSlice(SBValue val) {
    if (!val.IsValid()) return false;
    return val.GetChildMemberWithName("ptr").IsValid()
        && val.GetChildMemberWithName("len").IsValid();
}

bool IsZigArrayList(SBValue val) {
    if (!val.IsValid()) return false;
    SBValue items = val.GetChildMemberWithName("items");
    return items.IsValid() && IsZigSlice(items)
        && val.GetChildMemberWithName("capacity").IsValid();
}

bool IsZigOptional(SBValue val) {
    if (!val.IsValid()) return false;
    // Zig optionals have both 'some' (discriminant) and 'data' (payload) fields
    return val.GetChildMemberWithName("some").IsValid()
        && val.GetChildMemberWithName("data").IsValid();
}

bool IsZigErrorUnion(SBValue val) {
    if (!val.IsValid()) return false;
    // Zig error unions have 'tag' (error code) and 'value' (payload) fields
    return val.GetChildMemberWithName("tag").IsValid()
        && val.GetChildMemberWithName("value").IsValid();
}

// Generic regex-based transformer
using TransformFn = std::function<std::string(const std::smatch&, SBFrame)>;

std::string ApplyRegexTransform(
    const std::string& input,
    const std::regex& pattern,
    SBFrame frame,
    TransformFn transform
) {
    std::string result;
    std::string::const_iterator search_start = input.cbegin();
    size_t last_end = 0;

    std::smatch match;
    while (std::regex_search(search_start, input.cend(), match, pattern)) {
        size_t match_start = (search_start - input.cbegin()) + match.position();

        // Try transformation
        std::string replacement = transform(match, frame);
        if (!replacement.empty()) {
            result += input.substr(last_end, match_start - last_end);
            result += replacement;
            last_end = match_start + match.length();
        }
        search_start = match.suffix().first;
    }

    if (last_end == 0) return input;  // No transformations
    result += input.substr(last_end);
    return result;
}

} // anonymous namespace

static std::string TransformZigExpression(const std::string& expr, SBFrame frame) {
    std::string result = expr;

    // 1. Transform subscript: slice[n] -> slice.ptr[n], arraylist[n] -> arraylist.items.ptr[n]
    static const std::regex subscript_pattern(R"(([\w.]+)\s*\[([^\]]+)\])");
    result = ApplyRegexTransform(result, subscript_pattern, frame,
        [](const std::smatch& m, SBFrame f) -> std::string {
            std::string path = m[1].str();
            std::string index = m[2].str();
            SBValue val = GetValueAtPath(f, path);

            if (IsZigSlice(val)) {
                return path + ".ptr[" + index + "]";
            }
            if (IsZigArrayList(val)) {
                return path + ".items.ptr[" + index + "]";
            }
            return "";  // No transformation
        });

    // 2. Transform optional unwrap: optional.? -> optional.data (with null check)
    // Note: Zig optionals have 'some' (discriminant) and 'data' (payload) fields
    static const std::regex optional_pattern(R"(([\w.]+)\s*\.\s*\?)");
    result = ApplyRegexTransform(result, optional_pattern, frame,
        [](const std::smatch& m, SBFrame f) -> std::string {
            std::string path = m[1].str();
            SBValue val = GetValueAtPath(f, path);

            if (IsZigOptional(val)) {
                // Transform to: (path.some ? path.data : <error>)
                // For simplicity, just access .data - user should check null first
                return path + ".data";
            }
            return "";
        });

    // 3. Transform error catch: err catch default -> (err.tag == 0 ? err.value : default)
    static const std::regex catch_pattern(R"(([\w.]+)\s+catch\s+([\w.]+))");
    result = ApplyRegexTransform(result, catch_pattern, frame,
        [](const std::smatch& m, SBFrame f) -> std::string {
            std::string path = m[1].str();
            std::string dflt = m[2].str();
            SBValue val = GetValueAtPath(f, path);

            if (IsZigErrorUnion(val)) {
                return "(" + path + ".tag == 0 ? " + path + ".value : " + dflt + ")";
            }
            return "";
        });

    return result;
}

//===----------------------------------------------------------------------===//
// Custom Expression Command (overrides 'p')
//===----------------------------------------------------------------------===//

class ZigExpressionCommand : public SBCommandPluginInterface {
public:
    bool DoExecute(SBDebugger debugger, char** command, SBCommandReturnObject& result) override {
        // Reconstruct the expression from command arguments
        std::string expr;
        if (command) {
            for (int i = 0; command[i] != nullptr; ++i) {
                if (i > 0) expr += " ";
                expr += command[i];
            }
        }

        if (expr.empty()) {
            result.SetError("error: no expression provided");
            return false;
        }

        // Get current frame
        SBTarget target = debugger.GetSelectedTarget();
        if (!target.IsValid()) {
            result.SetError("error: no target");
            return false;
        }

        SBProcess process = target.GetProcess();
        if (!process.IsValid()) {
            result.SetError("error: no process");
            return false;
        }

        SBThread thread = process.GetSelectedThread();
        if (!thread.IsValid()) {
            result.SetError("error: no thread");
            return false;
        }

        SBFrame frame = thread.GetSelectedFrame();
        if (!frame.IsValid()) {
            result.SetError("error: no frame");
            return false;
        }

        // Transform Zig expressions to C
        std::string transformed = TransformZigExpression(expr, frame);

        // Evaluate the transformed expression
        SBExpressionOptions options;
        options.SetTimeoutInMicroSeconds(5000000);  // 5 seconds

        SBValue value = frame.EvaluateExpression(transformed.c_str(), options);

        if (value.GetError().Fail()) {
            // If transformation didn't help, try original expression
            if (transformed != expr) {
                value = frame.EvaluateExpression(expr.c_str(), options);
            }
            if (value.GetError().Fail()) {
                result.SetError(value.GetError().GetCString());
                return false;
            }
        }

        // Format output like standard 'p' command
        SBStream stream;
        value.GetDescription(stream);
        result.AppendMessage(stream.GetData());
        result.SetStatus(eReturnStatusSuccessFinishResult);

        return true;
    }
};

// Global instance to prevent destruction
static ZigExpressionCommand* g_zig_expr_cmd = nullptr;

static void RegisterZigExpressionCommand(SBDebugger debugger) {
    SBCommandInterpreter interp = debugger.GetCommandInterpreter();
    if (!interp.IsValid()) return;

    // Create command handler
    g_zig_expr_cmd = new ZigExpressionCommand();

    // Register our command as __zdb_expr (internal name)
    interp.AddCommand("__zdb_expr", g_zig_expr_cmd,
        "Internal: Evaluate expression with Zig syntax support.");

    // Override the 'p' alias to use our Zig-aware expression evaluator
    // First, remove the existing 'p' alias
    SBCommandReturnObject result;
    interp.HandleCommand("command unalias p", result);

    // Now create 'p' as an alias to our command
    interp.HandleCommand("command alias p __zdb_expr", result);

    // Also add 'zig' subcommands for explicit usage
    SBCommand zig_cmd = interp.AddMultiwordCommand("zig", "Zig debugging commands");
    if (zig_cmd.IsValid()) {
        zig_cmd.AddCommand("print", g_zig_expr_cmd,
            "Evaluate expression with Zig syntax support.");
        zig_cmd.AddCommand("p", g_zig_expr_cmd,
            "Shorthand for 'zig print'.");
    }
}

//===----------------------------------------------------------------------===//
// Plugin Entry Point
//===----------------------------------------------------------------------===//

bool lldb::PluginInitialize(SBDebugger debugger) {
    bool success = RegisterWithInternalAPI(debugger);

    // Register Zig expression command (overrides 'p' transparently)
    RegisterZigExpressionCommand(debugger);

    if (success) {
        fprintf(stderr, "[zdb] Loaded %zu formatters + expression syntax\n",
                g_formatters.size());
        return true;
    }

    fprintf(stderr, "[zdb] Formatters failed, but expression syntax available\n");
    return true;
}
