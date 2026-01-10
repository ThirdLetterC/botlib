const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const sanitize = b.option(bool, "sanitize", "Enable ASan/UBSan/leak for C sources") orelse false;

    const exe_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });

    const exe = b.addExecutable(.{
        .name = "mybot",
        .root_module = exe_mod,
    });

    const base_flags = &[_][]const u8{
        "-std=c2x",
        "-g",
        "-ggdb",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
    };

    const sanitize_flags = &[_][]const u8{
        "-std=c2x",
        "-g",
        "-ggdb",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "-fsanitize=address,undefined,leak",
        "-fno-omit-frame-pointer",
    };

    const flags = if (sanitize) sanitize_flags else base_flags;

    exe.addCSourceFiles(.{
        .files = &.{
            "src/parson.c",
            "src/sds.c",
            "src/json_wrap.c",
            "src/sqlite_wrap.c",
            "src/botlib.c",
            "src/mybot.c",
        },
        .flags = flags,
    });

    exe.root_module.addIncludePath(b.path("src/include"));

    exe.linkSystemLibrary("pthread");
    exe.linkSystemLibrary("curl");
    exe.linkSystemLibrary("sqlite3");

    const arch = target.result.cpu.arch;
    if (arch == .aarch64 or arch == .arm or arch == .armeb or arch == .thumb or arch == .thumbeb) {
        exe.linkSystemLibrary("atomic");
    }

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the application");
    run_step.dependOn(&run_cmd.step);
}
