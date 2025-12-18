const std = @import("std");

const Color = enum { red, green, blue };

const Point = struct { x: i32, y: i32 };

const Shape = union(enum) {
    circle: f32,
    rectangle: struct { w: f32, h: f32 },
};

var sink: usize = 0;

fn keep(ptr: anytype) void {
    sink +%= @intFromPtr(ptr);
}

pub fn main() !void {
    // Strings and slices
    const greeting: []const u8 = "Hello, zdb!";
    const numbers: []const i32 = &.{ 1, 2, 3, 4, 5 };

    // Optional
    var maybe: ?i32 = 42;
    var nothing: ?i32 = null;

    // Enum and struct
    var color: Color = .blue;
    var point: Point = .{ .x = 100, .y = 200 };

    // Tagged union
    var shape: Shape = .{ .circle = 3.14 };

    // ArrayList
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    var list: std.ArrayListUnmanaged(i32) = .empty;
    defer list.deinit(gpa.allocator());
    try list.appendSlice(gpa.allocator(), &.{ 10, 20, 30 });

    // Keep all variables alive
    keep(&greeting);
    keep(&numbers);
    keep(&maybe);
    keep(&nothing);
    keep(&color);
    keep(&point);
    keep(&shape);
    keep(&list);

    // Breakpoint on next line: b demo.zig:44
    std.debug.print("sink={}\n", .{sink});
}
