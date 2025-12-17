#ifndef ZDB_SHIM_H
#define ZDB_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to LLDB ValueObject
typedef void* ZdbValueObject;

// Callbacks from shim to Zig
typedef struct {
    const char* (*get_type_name)(ZdbValueObject valobj);
    size_t (*get_child_count)(ZdbValueObject valobj);
    ZdbValueObject (*get_child_at_index)(ZdbValueObject valobj, size_t index);
    ZdbValueObject (*get_child_by_name)(ZdbValueObject valobj, const char* name);
    uint64_t (*get_uint)(ZdbValueObject valobj);
    uint64_t (*get_address)(ZdbValueObject valobj);
    size_t (*read_memory)(uint64_t addr, uint8_t* buf, size_t size);
} ZdbShimCallbacks;

// Functions exported by Zig
extern void zdb_init(const ZdbShimCallbacks* callbacks);
extern bool zdb_format_slice(ZdbValueObject valobj, char* buf, size_t buf_size);
extern bool zdb_format_optional(ZdbValueObject valobj, char* buf, size_t buf_size);
extern bool zdb_format_error_union(ZdbValueObject valobj, char* buf, size_t buf_size);
extern size_t zdb_slice_num_children(ZdbValueObject valobj);
extern bool zdb_slice_get_child_name(size_t index, char* buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif // ZDB_SHIM_H
