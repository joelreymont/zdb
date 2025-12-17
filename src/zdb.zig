const std = @import("std");

// C interface for shim
pub const ValueObject = opaque {};

// Callbacks provided by shim for accessing LLDB ValueObjects
pub const ShimCallbacks = extern struct {
    get_type_name: *const fn (*ValueObject) callconv(.c) [*:0]const u8,
    get_child_count: *const fn (*ValueObject) callconv(.c) usize,
    get_child_at_index: *const fn (*ValueObject, usize) callconv(.c) ?*ValueObject,
    get_child_by_name: *const fn (*ValueObject, [*:0]const u8) callconv(.c) ?*ValueObject,
    get_uint: *const fn (*ValueObject) callconv(.c) u64,
    get_address: *const fn (*ValueObject) callconv(.c) u64,
    read_memory: *const fn (u64, [*]u8, usize) callconv(.c) usize,
};

var callbacks: ?ShimCallbacks = null;

/// Called by shim to initialize
export fn zdb_init(cb: *const ShimCallbacks) void {
    callbacks = cb.*;
    std.debug.print("zdb: initialized\n", .{});
}

/// Format a Zig slice type
export fn zdb_format_slice(valobj: *ValueObject, buf: [*]u8, buf_size: usize) bool {
    const cb = callbacks orelse return false;

    const ptr_child = cb.get_child_by_name(valobj, "ptr") orelse return false;
    const len_child = cb.get_child_by_name(valobj, "len") orelse return false;

    const ptr_val = cb.get_address(ptr_child);
    const len_val = cb.get_uint(len_child);

    const result = std.fmt.bufPrint(buf[0..buf_size], "len={d} ptr=0x{x}", .{ len_val, ptr_val }) catch return false;

    // Null terminate
    if (result.len < buf_size) {
        buf[result.len] = 0;
    }
    return true;
}

/// Format a Zig optional type
export fn zdb_format_optional(valobj: *ValueObject, buf: [*]u8, buf_size: usize) bool {
    const cb = callbacks orelse return false;

    // Zig optionals have a "some" child if non-null
    if (cb.get_child_by_name(valobj, "some")) |_| {
        const result = std.fmt.bufPrint(buf[0..buf_size], "(has value)", .{}) catch return false;
        if (result.len < buf_size) buf[result.len] = 0;
        return true;
    }

    const result = std.fmt.bufPrint(buf[0..buf_size], "null", .{}) catch return false;
    if (result.len < buf_size) buf[result.len] = 0;
    return true;
}

/// Format a Zig error union type
export fn zdb_format_error_union(valobj: *ValueObject, buf: [*]u8, buf_size: usize) bool {
    const cb = callbacks orelse return false;

    // Check if error is set
    if (cb.get_child_by_name(valobj, "error")) |err_child| {
        const err_val = cb.get_uint(err_child);
        if (err_val != 0) {
            const result = std.fmt.bufPrint(buf[0..buf_size], "error({d})", .{err_val}) catch return false;
            if (result.len < buf_size) buf[result.len] = 0;
            return true;
        }
    }

    const result = std.fmt.bufPrint(buf[0..buf_size], "(has value)", .{}) catch return false;
    if (result.len < buf_size) buf[result.len] = 0;
    return true;
}

// Synthetic children for slices
export fn zdb_slice_num_children(valobj: *ValueObject) usize {
    const cb = callbacks orelse return 0;

    const len_child = cb.get_child_by_name(valobj, "len") orelse return 0;
    return @intCast(cb.get_uint(len_child));
}

export fn zdb_slice_get_child_name(index: usize, buf: [*]u8, buf_size: usize) bool {
    const result = std.fmt.bufPrint(buf[0..buf_size], "[{d}]", .{index}) catch return false;
    if (result.len < buf_size) buf[result.len] = 0;
    return true;
}

test "basic" {
    // Placeholder
}
