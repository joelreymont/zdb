const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Zig module for core logic
    const zdb_mod = b.createModule(.{
        .root_source_file = b.path("src/zdb.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Shared library
    const lib = b.addLibrary(.{
        .linkage = .dynamic,
        .name = "zdb",
        .root_module = zdb_mod,
    });

    // Add C++ shim with native callbacks (uses SBTypeSummary::CreateWithCallback)
    lib.addCSourceFile(.{
        .file = b.path("shim/shim_callback.cpp"),
        .flags = &.{
            "-std=c++17",
            "-fno-exceptions",
        },
    });
    lib.addIncludePath(b.path("shim"));

    // LLDB headers and library
    const llvm_path = "/opt/homebrew/opt/llvm";
    lib.addSystemIncludePath(.{ .cwd_relative = llvm_path ++ "/include" });
    lib.addLibraryPath(.{ .cwd_relative = llvm_path ++ "/lib" });
    lib.linkSystemLibrary("lldb");
    lib.linkLibC();
    lib.linkLibCpp();

    b.installArtifact(lib);

    // Tests
    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/zdb.zig"),
        .target = target,
        .optimize = optimize,
    });
    const tests = b.addTest(.{
        .root_module = test_mod,
    });

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_tests.step);
}
