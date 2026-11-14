## SongoBGPlayer

Songo's music engine and background player, outside Godot's audio, so music keeps playing when Songo isn't running. It's the half of the old `songo_player_harness` that isn't Godot code; the Godot side, the `SongoPlayerHarness` node, is now a module in Songodot (`../Songodot/modules/songo_player_harness`).

- **`libsongo_engine`** (`engine/`). FFmpeg decoding, the play queue (looping, repeat-one, blending), Songo's EQ and volume, and audio output, all on its own thread behind a C API (`engine/include/songo_engine.h`). It publishes playback status, a spectrum and a waveform to shared memory (`engine/include/songo_shared.h`).
- **`songo_bgplayer`** (`daemon/`). Runs the engine in its own process, so music outlives Songo on Linux handhelds and desktops. Songo controls it over a Unix socket and maps its shared memory. When no client is connected, the handheld controls work instead.
- **The Android plugin** (`android/plugin/`). A Godot v2 plugin AAR that keeps the in-process engine playing on Android with the screen off or another app in front: a foreground media playback service with the notification, a media session for the lock screen, headsets and Bluetooth, audio focus, pausing when headphones are unplugged, and wake locks. It drives the engine through JNI (`engine/src/jni_android.cpp`), so it keeps working while Godot's activity is paused. The AAR also carries `libsongo_engine.android.arm64.so`, which is how the engine gets into an APK.

```
        Songo (Songodot)
   SongoPlayerHarness node (module)
     |                 |
  in-process        daemon transport ---- Unix socket (commands/events) ----+
  (Android,          |                                                      |
   debugging)        +-- mmap'd memfd (status, spectrum, waveform) --+      |
     |                                                               |      |
 libsongo_engine, dlopen'ed                         songo_bgplayer --> libsongo_engine
 into Godot's process                               (outlives Songo)
```

FFmpeg (and its mbedTLS) live only in `libsongo_engine.<platform>.<arch>.so` (`.dll` on Windows), hidden behind the C API by a version script (on Windows, by exporting only the C API). The daemon links it from its own directory (`$ORIGIN`). Songodot doesn't link it at all: the module dlopens it the first time in-process playback is needed, so Godot's own mbedTLS never meets FFmpeg's, and a Songodot without the library still runs and plays through the daemon.

### What the Songodot module shares with this repo
The module keeps copies of `engine/include/songo_engine.h`, `engine/include/songo_shared.h` and `common/protocol.{h,cpp}` in `modules/songo_player_harness/bgplayer/`. Change them here and copy them over. Mismatches are caught at runtime rather than misbehaving: the module checks `SONGO_ENGINE_API_VERSION` when it loads the engine, `SHARED_VERSION` when it maps shared memory, and `PROTOCOL_VERSION` in the daemon's `hello` (an older daemon is asked to quit and a new one started).

### Building
Clone with `git clone --recurse-submodules` (FFmpeg n8.1.2 and mbedTLS v3.6.7 are pinned submodules), then run the script for what you're building. Each builds mbedTLS and FFmpeg the first time, then the engine and daemon, into `bin/`:

| Script | Builds | For |
|---|---|---|
| `scripts/build_x86_64.sh` | `libsongo_engine.linux.x86_64.so`, `songo_bgplayer.linux.x86_64` | desktops, running Songo from the editor |
| `scripts/build_arm64.sh` | `libsongo_engine.linux.arm64.so`, `songo_bgplayer.linux.arm64` | arm64 Linux handhelds (PortMaster) |
| `scripts/build_android.sh` | `libsongo_engine.android.arm64.so`, and the plugin AAR (`android/plugin/build/outputs/aar/songo_harness-release.aar`) with it inside | Android (8.0+, for AAudio) |
| `scripts/build_windows.sh` | `libsongo_engine.windows.x86_64.dll` | 64-bit Windows (WASAPI) |

- `--rebuild-deps` as the first argument rebuilds mbedTLS and FFmpeg too, eg: after updating a submodule or `scripts/deps/ffmpeg_audio_components.sh` (which trims FFmpeg to audio only). FFmpeg builds inside `third_party/ffmpeg`, so don't run two builds at once.
- Anything after that goes to SCons, eg: `target=debug` (unoptimized, with symbols) or `use_llvm=yes` (clang, Linux).
- **x86_64** needs the ALSA headers (`libasound2-dev`), cmake and a C/C++ toolchain. libasound is linked dynamically, libstdc++/libgcc statically.
- **arm64**, on an x86_64 machine, cross compiles in Docker (`scripts/docker/Dockerfile`, image `songo-bgplayer-arm64`, or `$SONGO_ARM64_IMAGE`). It runs as your user with the repo mounted at `/src`, so `build/` and `bin/` stay yours. The image is Ubuntu 20.04 with its `aarch64-linux-gnu` GCC 9.4, which keeps the binaries needing only glibc 2.29: don't move it to a newer release without checking the oldest CFW you support. On an aarch64 machine (eg: an arm64 chroot) the script builds natively instead, and that machine's glibc sets the floor.
- **Android** needs NDK r23c (`ANDROID_NDK_ROOT`, or `ANDROID_HOME` with `ndk/23.2.8568313`), and for the AAR the SDK (`ANDROID_HOME`) and a JDK 17 (`JAVA_HOME`, or `~/Android/jdk17`). The AAR uses the same Gradle 8.2, AGP 8.2.0 and SDK 34 as Godot 4.3's build template. `--no-aar` stops after the engine library. Before Android 8.0 there's no AAudio, so the engine doesn't start.
- **Windows** cross compiles with MinGW-w64 (posix threads) in Docker (`scripts/docker/Dockerfile.windows`, image `songo-bgplayer-windows`, or `$SONGO_WINDOWS_IMAGE`). The DLL links libgcc, libstdc++ and libwinpthread statically and exports only the C API, so it needs nothing beyond Windows' own DLLs. There's no daemon on Windows: Songodot plays through the DLL in-process, and music stops with Songo.
- `scripts/deps/build_mbedtls.sh <target>` and `scripts/deps/build_ffmpeg.sh <target>` (`x86_64`, `arm64`, `android-arm64` or `windows-x86_64`) build one dependency on its own.

After building, copy the Linux and Windows engine builds and the daemons into Songo-5's `addons/songo_player_harness/bin`, and the AAR into its `addons/songo_player_harness/android`.

### Using it with Songodot
`SongoPlayerHarness` is built into Songodot (the `songo_player_harness` module, on in `sbc_profile.py` and `android_profile.py`). Unless its `engine_library_path` and `daemon_path` are set, it looks for `libsongo_engine.<platform>.<arch>.so` and `songo_bgplayer.<platform>.<arch>`:
1. next to the Godot executable, which is where exports put them (Songo-5's PortMaster build copies them beside the runtime)
2. in the project's `addons/songo_player_harness/bin`, when running from the project's folder (the editor, desktop debugging). Give that folder a `.gdignore` so Godot doesn't import or export it.

On Android the engine comes from the APK's native libraries. The app needs the AAR and an `EditorExportPlugin` that returns it from `_get_android_libraries()` (see Songo-5's `addons/songo_player_harness/plugin.gd`), with **Use Gradle Build** on in the export preset, since a plain template export can't add a service. The AAR's manifest brings the service and the `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PLAYBACK` and `WAKE_LOCK` permissions. Once loaded, the plugin follows the engine by itself: it starts the foreground service (so Android keeps the process running), takes audio focus and a wake lock, adds a Wi-Fi lock for streams, and shows the song's own tags and cover art (read with `MediaMetadataRetriever`) in the notification and on the lock screen.

```gdscript
var player := SongoPlayerHarness.new()
add_child(player)
player.loop_queue = true
player.set_queue(paths, start_index, start_seconds)  # autoplay by default
player.finished.connect(_on_queue_end)                # queue ran out (looping off)
player.track_changed.connect(_on_track_changed)       # (index, path), when it's heard
```

- **Playback:** `play(path, seek_time)`, `set_queue(paths, start_index, start_position, autoplay)`, `replace_queue(paths, current_index)` (reorder/shuffle without interrupting the current song), `play_index`, `pause`, `resume`, `stop`, `seek`, `next`, `previous` (restarts the song, or goes back one within its first 3s).
- **State:** `get_state`, `is_playing`, `is_paused`, `is_live`, `is_seekable`, `get_playback_position`, `get_duration`, `get_current_index`, `get_queue`, `get_stream_title`. These read shared memory, so they're cheap to call every frame.
- **Settings:** `volume`, `blend_time`, `repeat_one`, `loop_queue`, `eq_enabled`, `eq_band_gains` (10 bands in dB, Godot's `AudioEffectEQ10`), `network_timeout`, `user_agent`, `skip_failed` (on by default: songs that won't open are skipped; off, the engine reports an error and stops so the app can decide), `output_latency_ms` (the output buffer, 200ms to start), `stream_buffer_length` (seconds network streams decode ahead on their own thread, 1s to start).
- **Signals:** `finished`, `error(message)`, `stream_title_changed(title)`, `track_changed(index, path)`, `state_changed(state)`, `queue_changed`, `engine_connection_changed(connected)`.
- **Where the engine runs:** `transport` (Auto picks the daemon on Linux and in-process on Android and Windows, which has no daemon), `socket_path`, `daemon_path`, `engine_library_path`, `output_backend` (`alsa`, `aaudio`, `wasapi`, or `null` for silent testing), `output_device`, `daemon_exits_with_app`, `daemon_exit_hook`.

When the node is ready, it connects to a daemon that's already playing and receives the queue, song, title and state. It only starts a daemon once a playback command needs one. Settings set before that wait for the connection, so loading a scene never starts the daemon.

`get_magnitude_for_frequency_range(from_hz, to_hz, mode)` matches `AudioEffectSpectrumAnalyzerInstance`'s (same FFT size, window, normalization and bin picking; `tests` checks it against Godot's analyzer to within 1dB), for the audio after volume and blend, before the EQ, timestamped to when it reaches the speaker. `get_waveform(frame_count)` returns the stereo frames just heard.

### The daemon
```
songo_bgplayer [playlist.m3u] [--blend SECONDS] [--idle] [--socket PATH] [--backend alsa|null]
               [--device NAME] [--no-input] [--exit-on-disconnect] [--log FILE]
```
- **Playlists:** given one, it plays it straight away, with the `#SONGO-*` lines plus `#SONGO-LOOP:0`. If a daemon is already running, the playlist is handed to that one instead.
- **Single instance:** one daemon per socket, enforced by a lock file next to it. The default socket is `$SONGO_PLAYER_SOCKET`, else `$XDG_RUNTIME_DIR/songo_player.sock`, or `/tmp/songo_player-<uid>.sock` without one.
- **Controls:** the handheld controls (hold Select to quit, Select+Left/Right to skip, Select+Up/Down for volume) only work while no client is connected.
- **When it quits:** on Select held, SIGTERM, or a `quit` command; when nothing is playing and nobody has been connected for 3 seconds (10 seconds after an `--idle` start); with `--exit-on-disconnect`, as soon as the last client leaves.
- **Protocol:** tab-separated text lines, documented in `common/protocol.h`. Poke at it with `socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/songo_player.sock`. On connect it sends `hello` with the shared memory's memfd attached, then a snapshot of the queue, song, title and state.

### Tests
`tests/run_tests.sh <songodot binary>` builds a scratch project and runs headless checks with the Songodot editor binary (anything with the `songo_player_harness` module) against what's in `bin/`: playback in-process and through the daemon, the spectrum against Godot's analyzer, blending, failures, latency changes, a stalling network stream, and leaving Songo while music plays and coming back. They use the null backend, so they're silent; `tests/run_tests.sh <songodot> alsa` also tries the real ALSA device, at volume 0.
