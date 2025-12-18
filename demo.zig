const std = @import("std");

const Color = enum { red, green, blue };
const Point = struct { x: i32, y: i32 };
const Shape = union(enum) {
    circle: f32,
    rectangle: struct { w: f32, h: f32 },
};

pub fn main() !void {
    const greeting: []const u8 = "Hello, zdb!";
    const numbers: []const i32 = &.{ 1, 2, 3, 4, 5 };
    var maybe: ?i32 = 42;
    var nothing: ?i32 = null;
    var color: Color = .blue;
    var point: Point = .{ .x = 100, .y = 200 };
    var shape: Shape = .{ .circle = 3.14 };

    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    var list: std.ArrayListUnmanaged(i32) = .empty;
    defer list.deinit(gpa.allocator());
    try list.appendSlice(gpa.allocator(), &.{ 10, 20, 30 });

    _ = .{ &greeting, &numbers, &maybe, &nothing, &color, &point, &shape, &list };
    std.debug.print("Ready\n", .{}); // breakpoint here
}
