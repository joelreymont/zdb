// zdb LLDB plugin shim
//
// This C++ code bridges between LLDB's plugin API and Zig implementations.
// LLDB's API is unstable C++, so we wrap it here and expose a stable C interface to Zig.

#include "shim.h"
#include <cstdio>
#include <cstring>

// LLDB public API (stable)
#include "lldb/API/SBDebugger.h"

//------------------------------------------------------------------------------
// Stub implementations for callbacks (until LLDB is integrated)
//------------------------------------------------------------------------------

static const char* stub_get_type_name(ZdbValueObject valobj) {
    (void)valobj;
    return "unknown";
}

static size_t stub_get_child_count(ZdbValueObject valobj) {
    (void)valobj;
    return 0;
}

static ZdbValueObject stub_get_child_at_index(ZdbValueObject valobj, size_t index) {
    (void)valobj;
    (void)index;
    return nullptr;
}

static ZdbValueObject stub_get_child_by_name(ZdbValueObject valobj, const char* name) {
    (void)valobj;
    (void)name;
    return nullptr;
}

static uint64_t stub_get_uint(ZdbValueObject valobj) {
    (void)valobj;
    return 0;
}

static uint64_t stub_get_address(ZdbValueObject valobj) {
    (void)valobj;
    return 0;
}

static size_t stub_read_memory(uint64_t addr, uint8_t* buf, size_t size) {
    (void)addr;
    (void)buf;
    (void)size;
    return 0;
}

static ZdbShimCallbacks g_callbacks = {
    .get_type_name = stub_get_type_name,
    .get_child_count = stub_get_child_count,
    .get_child_at_index = stub_get_child_at_index,
    .get_child_by_name = stub_get_child_by_name,
    .get_uint = stub_get_uint,
    .get_address = stub_get_address,
    .read_memory = stub_read_memory,
};

//------------------------------------------------------------------------------
// Plugin initialization (called when dylib is loaded)
//------------------------------------------------------------------------------

__attribute__((constructor))
static void zdb_plugin_init() {
    fprintf(stderr, "[zdb] Plugin loaded\n");

    // Initialize Zig side
    zdb_init(&g_callbacks);

    // TODO: Register formatters with LLDB
    // This requires LLDB headers and linking against liblldb
    //
    // Example (pseudo-code):
    // auto& category = debugger.GetCategory("zig");
    // category.AddTypeSummary("^\\[\\].*$", make_shared<ZigSliceSummary>());

    fprintf(stderr, "[zdb] Formatters registered (stub)\n");
}

__attribute__((destructor))
static void zdb_plugin_fini() {
    fprintf(stderr, "[zdb] Plugin unloaded\n");
}

//------------------------------------------------------------------------------
// LLDB Plugin API
//------------------------------------------------------------------------------

namespace lldb {
    bool PluginInitialize(SBDebugger debugger) {
        (void)debugger;
        fprintf(stderr, "[zdb] PluginInitialize called\n");
        // TODO: Register type formatters with debugger
        return true;
    }

    void PluginTerminate() {
        fprintf(stderr, "[zdb] PluginTerminate called\n");
    }
}

//------------------------------------------------------------------------------
// LLDB integration (requires LLDB headers)
//------------------------------------------------------------------------------

#if 0 // Enable when LLDB headers are available

#include "lldb/lldb-public.h"
#include "lldb/DataFormatters/TypeSummary.h"
#include "lldb/DataFormatters/TypeSynthetic.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Symbol/CompilerType.h"

using namespace lldb;
using namespace lldb_private;

// Wrapper callbacks that call into real LLDB ValueObjects
static const char* real_get_type_name(ZdbValueObject valobj) {
    auto* vo = static_cast<ValueObject*>(valobj);
    return vo->GetTypeName().GetCString();
}

static size_t real_get_child_count(ZdbValueObject valobj) {
    auto* vo = static_cast<ValueObject*>(valobj);
    return vo->GetNumChildren();
}

static ZdbValueObject real_get_child_at_index(ZdbValueObject valobj, size_t index) {
    auto* vo = static_cast<ValueObject*>(valobj);
    return vo->GetChildAtIndex(index).get();
}

static ZdbValueObject real_get_child_by_name(ZdbValueObject valobj, const char* name) {
    auto* vo = static_cast<ValueObject*>(valobj);
    return vo->GetChildMemberWithName(name).get();
}

static uint64_t real_get_uint(ZdbValueObject valobj) {
    auto* vo = static_cast<ValueObject*>(valobj);
    bool success = false;
    return vo->GetValueAsUnsigned(0, &success);
}

static uint64_t real_get_address(ZdbValueObject valobj) {
    auto* vo = static_cast<ValueObject*>(valobj);
    return vo->GetPointerValue();
}

// TypeSummary implementation that delegates to Zig
class ZigSliceSummary : public TypeSummaryImpl {
public:
    ZigSliceSummary() : TypeSummaryImpl(TypeSummaryImpl::Flags()) {}

    bool FormatObject(ValueObject* valobj, std::string& dest,
                      const TypeSummaryOptions& options) override {
        char buf[256];
        if (zdb_format_slice(valobj, buf, sizeof(buf))) {
            dest = buf;
            return true;
        }
        return false;
    }

    std::string GetDescription() override { return "Zig slice formatter"; }
};

// SyntheticChildren implementation for slices
class ZigSliceSynthetic : public SyntheticChildrenFrontEnd {
    ValueObject& m_backend;
    size_t m_num_children;

public:
    ZigSliceSynthetic(ValueObject& backend)
        : SyntheticChildrenFrontEnd(backend), m_backend(backend), m_num_children(0) {}

    size_t CalculateNumChildren() override {
        m_num_children = zdb_slice_num_children(&m_backend);
        return m_num_children;
    }

    ValueObjectSP GetChildAtIndex(size_t idx) override {
        // TODO: Create synthetic child for slice[idx]
        return nullptr;
    }

    bool Update() override {
        m_num_children = zdb_slice_num_children(&m_backend);
        return true;
    }

    bool MightHaveChildren() override { return true; }

    size_t GetIndexOfChildWithName(ConstString name) override {
        // Parse "[N]" format
        return UINT32_MAX;
    }
};

#endif // LLDB headers available
