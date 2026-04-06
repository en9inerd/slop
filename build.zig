const std = @import("std");

const lib_sources = &.{
    "src/api.c",
    "src/lang.c",
    "src/scan.c",
    "src/smell.c",
    "src/score.c",
    "src/compress.c",
    "src/dupes.c",
    "src/git.c",
    "src/util.c",
    "src/walk.c",
};

const c_flags = &.{
    "-std=c23",
    "-D_POSIX_C_SOURCE=200809L",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Wconversion",
    "-Wsign-conversion",
    "-Wshadow",
    "-Wdouble-promotion",
    "-Wformat=2",
    "-Wnull-dereference",
    "-Wimplicit-fallthrough",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
};

fn linkZlib(mod: *std.Build.Module, b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) void {
    if (b.lazyDependency("zlib", .{ .target = target, .optimize = optimize })) |zlib_dep| {
        mod.linkLibrary(zlib_dep.artifact("z"));
    } else {
        mod.linkSystemLibrary("z", .{});
    }
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const version = b.option([]const u8, "version", "Version string") orelse "dev";

    // ── Static library (libsad.a) ────────────────────────────
    const lib = b.addLibrary(.{
        .name = "slop",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    lib.root_module.addCSourceFiles(.{
        .root = b.path(""),
        .files = lib_sources,
        .flags = c_flags,
    });

    linkZlib(lib.root_module, b, target, optimize);

    b.installArtifact(lib);
    b.installFile("src/slop.h", "include/slop.h");

    // ── Main executable ──────────────────────────────────────
    const exe = b.addExecutable(.{
        .name = "slop",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    exe.root_module.addCMacro("SLOP_VERSION", b.fmt("\"{s}\"", .{version}));
    exe.root_module.addCSourceFiles(.{
        .root = b.path(""),
        .files = &.{"src/main.c"},
        .flags = c_flags,
    });

    exe.root_module.linkLibrary(lib);
    linkZlib(exe.root_module, b, target, optimize);

    b.installArtifact(exe);

    // ── Run step: zig build run -- <args> ────────────────────
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);

    const run_step = b.step("run", "Run slop with arguments");
    run_step.dependOn(&run_cmd.step);

    // ── Test step: zig build test ────────────────────────────
    const test_runner = b.addSystemCommand(&.{ "sh", "tests/run.sh" });
    test_runner.step.dependOn(b.getInstallStep());
    test_runner.setEnvironmentVariable("SLOP_BIN", b.getInstallPath(.bin, "slop"));

    const test_step = b.step("test", "Run integration tests");
    test_step.dependOn(&test_runner.step);
}
