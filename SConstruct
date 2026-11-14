#!/usr/bin/env python
#
# Builds, for the given platform/arch:
#   bin/libsongo_engine.<platform>.<arch>.so   the engine, with FFmpeg inside
#                                              (.dll on Windows)
#   bin/songo_bgplayer.<platform>.<arch>       the daemon (Linux only)
#
#   scons platform=linux arch=x86_64 [target=release|debug] [use_llvm=yes]
#   scons platform=android arch=arm64
#   scons platform=windows arch=x86_64         (MinGW-w64, see scripts/build_windows.sh)
#
# The daemon links the engine library and finds it next to itself ($ORIGIN),
# so FFmpeg ships once. Songo's SongoPlayerHarness (a Songodot module, see
# ../Songodot/modules/songo_player_harness) loads the same library at runtime
# for in-process playback. On Android the engine also carries the JNI the
# plugin's playback service calls (android/plugin), whose AAR bundles it.
#
# Usually run through scripts/build_x86_64.sh, build_arm64.sh or
# build_android.sh, which build mbedTLS and FFmpeg first (scripts/deps/).
#
# Cross compiling for arm64 Linux: scripts/build_arm64.sh does it in Docker,
# or set CROSS_PREFIX (eg: aarch64-linux-gnu-) in the environment.
# Android needs ANDROID_NDK_ROOT, or ANDROID_HOME with NDK r23c
# (23.2.8568313) installed under it.

import os

opts = Variables()
opts.Add(EnumVariable("platform", "Target platform", "linux", ("linux", "android", "windows")))
opts.Add(EnumVariable("arch", "Target architecture", "x86_64", ("x86_64", "arm64")))
opts.Add(EnumVariable("target", "Optimized release or debug build", "release", ("release", "debug")))
opts.Add(BoolVariable("use_llvm", "Use clang instead of gcc (Linux)", False))
opts.Add("android_api_level", "Minimum Android API level", "21")
opts.Add("ndk_version", "NDK under $ANDROID_HOME/ndk, when ANDROID_NDK_ROOT isn't set", "23.2.8568313")

env = Environment(variables=opts, ENV=os.environ)
Help(opts.GenerateHelpText(env))

platform = env["platform"]
arch = env["arch"]
project_root = Dir("#").abspath

if platform == "android" and arch != "arm64":
    raise RuntimeError("Only arm64 is built for Android (see scripts/deps/build_ffmpeg.sh)")
if platform == "windows" and arch != "x86_64":
    raise RuntimeError("Only x86_64 is built for Windows (see scripts/deps/build_ffmpeg.sh)")


# --------------------------------------------------
# Toolchain
# --------------------------------------------------

if platform == "linux":
    prefix = os.environ.get("CROSS_PREFIX", "")
    if env["use_llvm"]:
        env["CC"] = prefix + "clang"
        env["CXX"] = prefix + "clang++"
        env["AR"] = prefix + "llvm-ar" if prefix else "llvm-ar"
        env["RANLIB"] = prefix + "llvm-ranlib" if prefix else "llvm-ranlib"
    else:
        env["CC"] = prefix + "gcc"
        env["CXX"] = prefix + "g++"
        env["AR"] = prefix + "ar"
        env["RANLIB"] = prefix + "ranlib"
    march = "-march=x86-64" if arch == "x86_64" else "-march=armv8-a"
    env.Append(CCFLAGS=[march], LINKFLAGS=[march])
elif platform == "windows":
    prefix = os.environ.get("CROSS_PREFIX", "x86_64-w64-mingw32-")
    env["CC"] = prefix + "gcc"
    env["CXX"] = prefix + "g++"
    env["AR"] = prefix + "ar"
    env["RANLIB"] = prefix + "ranlib"
    env["SHLIBSUFFIX"] = ".dll"
    # PE code is position independent anyway, and gcc warns about -fPIC
    env["SHCCFLAGS"] = "$CCFLAGS"
    env.Append(CCFLAGS=["-march=x86-64"], LINKFLAGS=["-march=x86-64"])
else:
    ndk = os.environ.get("ANDROID_NDK_ROOT") or os.path.join(os.environ.get("ANDROID_HOME", ""), "ndk", env["ndk_version"])
    toolchain = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin")
    if not os.path.isdir(toolchain):
        raise RuntimeError("Android NDK not found at {}, set ANDROID_NDK_ROOT or ANDROID_HOME".format(ndk))
    target = "--target=aarch64-linux-android" + env["android_api_level"]
    env["CC"] = os.path.join(toolchain, "clang")
    env["CXX"] = os.path.join(toolchain, "clang++")
    env["AR"] = os.path.join(toolchain, "llvm-ar")
    env["RANLIB"] = os.path.join(toolchain, "llvm-ranlib")
    env.Append(CCFLAGS=[target, "-march=armv8-a"], LINKFLAGS=[target, "-march=armv8-a"])

env.Append(CCFLAGS=["-ffunction-sections", "-fdata-sections", "-Wall"])
env.Append(CXXFLAGS=["-std=c++17"])
if platform != "windows":
    # On Windows the DLL exports only what's marked dllexport (SONGO_API)
    env.Append(CCFLAGS=["-fPIC", "-fvisibility=hidden"], CXXFLAGS=["-fvisibility-inlines-hidden"])
if env["target"] == "release":
    env.Append(CCFLAGS=["-O2"], CPPDEFINES=["NDEBUG"], LINKFLAGS=["-s"])
else:
    env.Append(CCFLAGS=["-O0", "-g"])


# --------------------------------------------------
# FFmpeg and mbedTLS (see scripts/deps/)
# --------------------------------------------------

# Android builds live in their own subdirectory so they don't clobber the
# Linux build for the same architecture.
sub = os.path.join(platform, arch) if platform in ("android", "windows") else arch
ffmpeg_dir = os.path.join(project_root, "build", "ffmpeg", sub)
mbedtls_dir = os.path.join(project_root, "build", "mbedtls", sub)

if not os.path.isfile(os.path.join(ffmpeg_dir, "lib", "libavformat.a")):
    raise RuntimeError("FFmpeg not found in {}, build it first: scripts/build_<x86_64|arm64|android>.sh".format(ffmpeg_dir))

# File nodes so these are linked as archives rather than -l names, in this
# order: libmbedtls depends on x509, which depends on crypto.
ffmpeg_static_libs = [env.File(os.path.join(ffmpeg_dir, "lib", name)) for name in ("libavformat.a", "libavcodec.a", "libswresample.a", "libavutil.a")]
ffmpeg_static_libs += [env.File(os.path.join(mbedtls_dir, "lib", name)) for name in ("libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a")]


# --------------------------------------------------
# Helpers
# --------------------------------------------------

# The same source (common/protocol.cpp) goes into more than one binary, so
# each binary gets its own object directory.
def objects(build_env, tag, sources):
    nodes = []
    for source in sources:
        name = os.path.splitext(os.path.basename(source))[0]
        target = os.path.join("build", "obj", "{}.{}.{}".format(platform, arch, env["target"]), tag, name)
        nodes.append(build_env.SharedObject(target=target, source=source))
    return nodes


engine_name = "libsongo_engine.{}.{}{}".format(platform, arch, env["SHLIBSUFFIX"])


# --------------------------------------------------
# libsongo_engine
# --------------------------------------------------

engine_env = env.Clone()
engine_env.Append(CPPPATH=[os.path.join(ffmpeg_dir, "include"), "engine/include", "engine/src"])
# Drop unused FFmpeg functions, which are built in their own sections (see
# scripts/deps/ffmpeg_audio_components.sh)
engine_env.Append(LINKFLAGS=["-Wl,--gc-sections"])
if platform != "windows":
    engine_env.Append(
        LINKFLAGS=[
            # FFmpeg's x86_64 CABAC code contains PC-relative references to
            # internal FFmpeg symbols. Treat symbols defined by this shared
            # library as non-preemptible.
            "-Wl,-Bsymbolic",
            "-Wl,-soname," + engine_name,
        ]
    )

engine_sources = [
    "engine/src/engine.cpp",
    "engine/src/output.cpp",
    "engine/src/output_null.cpp",
    "engine/src/song.cpp",
    "engine/src/spectrum_analyzer.cpp",
]

if platform == "linux":
    engine_sources.append("engine/src/output_alsa.cpp")
    engine_env.Append(CPPDEFINES=["SONGO_HAS_ALSA"])
    engine_env.Append(
        LINKFLAGS=[
            # Only songo_* leaves the library
            "-Wl,--version-script=" + os.path.join(project_root, "engine", "songo_engine.map"),
            # The engine doesn't depend on the CFW's C++ runtime, and its copy
            # stays hidden behind the C API
            "-static-libstdc++",
            "-static-libgcc",
        ]
    )
    engine_env.Append(LIBS=[*ffmpeg_static_libs, "asound", "pthread", "m", "dl"])
elif platform == "windows":
    engine_sources.append("engine/src/output_wasapi.cpp")
    # -static folds libgcc, libstdc++ and libwinpthread into the DLL, so it
    # needs nothing but Windows' own DLLs. ws2_32/bcrypt/secur32 are for
    # FFmpeg's network code and mbedTLS, ole32 for WASAPI's COM.
    engine_env.Append(LINKFLAGS=["-static"])
    engine_env.Append(LIBS=[*ffmpeg_static_libs, "ole32", "ws2_32", "bcrypt", "secur32", "m"])
else:
    engine_env.Append(CPPPATH=["common"])
    engine_sources += [
        # AAudio is loaded at runtime, it's missing before Android 8.0
        "engine/src/output_aaudio.cpp",
        "engine/src/jni_android.cpp",
        "common/protocol.cpp",
    ]
    # The C++ runtime is libc++_shared, which the APK already carries for Godot
    engine_env.Append(LINKFLAGS=["-Wl,--version-script=" + os.path.join(project_root, "engine", "songo_engine_android.map")])
    engine_env.Append(LIBS=[*ffmpeg_static_libs, "log", "android", "m", "dl"])

engine_lib = engine_env.SharedLibrary(
    os.path.join("bin", engine_name),
    source=objects(engine_env, "engine", engine_sources),
    SHLIBPREFIX="",
)
engine_env.Depends(engine_lib, ["engine/songo_engine.map", "engine/songo_engine_android.map"])

targets = [engine_lib]


# --------------------------------------------------
# songo_bgplayer
# --------------------------------------------------

if platform == "linux":
    daemon_env = env.Clone()
    daemon_env.Append(CPPPATH=["engine/include", "common", "daemon"])
    daemon_env.Append(LINKFLAGS=["-static-libstdc++", "-static-libgcc", "-Wl,-rpath,'$$ORIGIN'", "-Wl,--gc-sections"])
    daemon_env.Append(LIBS=[engine_lib, "pthread"])
    daemon = daemon_env.Program(
        os.path.join("bin", "songo_bgplayer.{}.{}".format(platform, arch)),
        source=objects(daemon_env, "daemon", ["daemon/main.cpp", "daemon/input.cpp", "common/protocol.cpp"]),
    )
    targets.append(daemon)

Default(targets)
