// Test program for zdb LLDB formatters
// Compile with: zig build-exe test/test_types.zig -femit-bin=test/test_types
// Debug with: lldb ./test_types -o "command script import zig_formatters.py"

const std = @import("std");

// Custom error set for testing
const MyError = error{
    InvalidInput,
    OutOfMemory,
    NetworkError,
};

// Tagged union for testing
const Shape = union(enum) {
    circle: f32, // radius
    rectangle: struct { width: f32, height: f32 },
    triangle: struct { base: f32, height: f32 },
    none,
};

// Simple struct for testing
const Point = struct {
    x: i32,
    y: i32,
};

// Larger struct for testing
const Person = struct {
    name: []const u8,
    age: u32,
    active: bool,
    score: f32,
    location: Point,
};

// Enum for testing
const Color = enum {
    red,
    green,
    blue,
    yellow,
};

// Struct with various types for testing
const TestStruct = struct {
    name: []const u8,
    values: []i32,
    optional_value: ?i32,
    error_result: MyError!i32,
    shape: Shape,
};

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // Test slices
    const string_slice: []const u8 = "Hello, zdb debugger!";
    const int_slice: []const i32 = &[_]i32{ 1, 2, 3, 4, 5 };

    // Test mutable slice
    var mutable_bytes = [_]u8{ 'a', 'b', 'c', 'd', 'e' };
    const byte_slice: []u8 = &mutable_bytes;

    // Test optionals
    const some_value: ?i32 = 42;
    const none_value: ?i32 = null;
    const some_pointer: ?*const i32 = &int_slice[0];
    const none_pointer: ?*const i32 = null;

    // Test error unions
    const success_result: MyError!i32 = 100;
    const error_result: MyError!i32 = MyError.InvalidInput;

    // Test tagged unions
    const circle: Shape = .{ .circle = 5.0 };
    const rectangle: Shape = .{ .rectangle = .{ .width = 10.0, .height = 20.0 } };
    const empty_shape: Shape = .none;

    // Test fixed-size arrays
    const fixed_array: [5]i32 = .{ 10, 20, 30, 40, 50 };
    const byte_array: [3]u8 = .{ 'a', 'b', 'c' };

    // Test structs
    const point: Point = .{ .x = 100, .y = 200 };
    const person: Person = .{
        .name = "Alice",
        .age = 30,
        .active = true,
        .score = 95.5,
        .location = .{ .x = 10, .y = 20 },
    };

    // Test enums
    const color: Color = .blue;

    // Test pointers
    const ptr_to_int: *const i32 = &fixed_array[2];
    const many_ptr: [*]const i32 = &fixed_array;

    // Test tuples
    const tuple = .{ @as(i32, 1), @as(f32, 2.5), true };

    // Test ArrayList (Zig 0.15 API)
    var list: std.ArrayListUnmanaged(i32) = .empty;
    defer list.deinit(allocator);
    try list.append(allocator, 10);
    try list.append(allocator, 20);
    try list.append(allocator, 30);

    // Test HashMap (Zig 0.15 API)
    var map: std.StringHashMapUnmanaged(i32) = .empty;
    defer map.deinit(allocator);
    try map.put(allocator, "one", 1);
    try map.put(allocator, "two", 2);
    try map.put(allocator, "three", 3);

    // Test C string (sentinel-terminated)
    const c_string: [*:0]const u8 = "C string test";

    // Test struct with all types
    var result_values = [_]i32{ 10, 20, 30 };
    const test_struct = TestStruct{
        .name = "test object",
        .values = &result_values,
        .optional_value = 42,
        .error_result = 100,
        .shape = .{ .circle = 3.14 },
    };

    // Prevent optimization - keep all variables alive
    std.mem.doNotOptimizeAway(&some_value);
    std.mem.doNotOptimizeAway(&none_value);
    std.mem.doNotOptimizeAway(&some_pointer);
    std.mem.doNotOptimizeAway(&none_pointer);
    std.mem.doNotOptimizeAway(&success_result);
    std.mem.doNotOptimizeAway(&error_result);
    std.mem.doNotOptimizeAway(&circle);
    std.mem.doNotOptimizeAway(&rectangle);
    std.mem.doNotOptimizeAway(&empty_shape);
    std.mem.doNotOptimizeAway(&fixed_array);
    std.mem.doNotOptimizeAway(&byte_array);
    std.mem.doNotOptimizeAway(&point);
    std.mem.doNotOptimizeAway(&person);
    std.mem.doNotOptimizeAway(&color);
    std.mem.doNotOptimizeAway(&ptr_to_int);
    std.mem.doNotOptimizeAway(&many_ptr);
    std.mem.doNotOptimizeAway(&tuple);
    std.mem.doNotOptimizeAway(&list);
    std.mem.doNotOptimizeAway(&map);
    std.mem.doNotOptimizeAway(&test_struct);
    std.mem.doNotOptimizeAway(&c_string);

    // Set a breakpoint here to inspect all values
    std.debug.print("Breakpoint here! Inspect variables with 'frame variable'\n", .{});

    // Use variables to prevent them from being optimized away
    std.debug.print("string_slice: {s}\n", .{string_slice});
    std.debug.print("int_slice len: {d}\n", .{int_slice.len});
    std.debug.print("byte_slice len: {d}\n", .{byte_slice.len});
    std.debug.print("some_value: {any}\n", .{some_value});
    std.debug.print("none_value: {any}\n", .{none_value});
    std.debug.print("some_pointer: {any}\n", .{some_pointer});
    std.debug.print("none_pointer: {any}\n", .{none_pointer});
    std.debug.print("success_result: {any}\n", .{success_result});
    std.debug.print("error_result: {any}\n", .{error_result});
    std.debug.print("circle: {any}\n", .{circle});
    std.debug.print("rectangle: {any}\n", .{rectangle});
    std.debug.print("empty_shape: {any}\n", .{empty_shape});
    std.debug.print("list len: {d}\n", .{list.items.len});
    std.debug.print("map count: {d}\n", .{map.size});
    std.debug.print("test_struct.name: {s}\n", .{test_struct.name});

    // Use new variables
    std.debug.print("fixed_array[0]: {d}\n", .{fixed_array[0]});
    std.debug.print("byte_array: {s}\n", .{&byte_array});
    std.debug.print("point: ({d}, {d})\n", .{ point.x, point.y });
    std.debug.print("person.name: {s}\n", .{person.name});
    std.debug.print("color: {}\n", .{color});
    std.debug.print("ptr_to_int: {d}\n", .{ptr_to_int.*});
    std.debug.print("many_ptr[0]: {d}\n", .{many_ptr[0]});
    std.debug.print("tuple[0]: {d}\n", .{tuple[0]});
    std.debug.print("c_string: {s}\n", .{c_string});
}
