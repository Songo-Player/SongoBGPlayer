# Headless checks for SongoPlayerHarness, run by tests/run_tests.sh:
#
#   godot --headless -s test.gd -- <mode> <audio dir> <socket path>
#
# Modes:
#   inproc          playback, seek, pause, queue, volume, EQ, stop, in-process
#   daemon          the same through songo_bgplayer, which it starts itself
#   compare         our spectrum next to Godot's AudioEffectSpectrumAnalyzer
#   blend           crossfading between two songs
#   handoff_leave   start playing through the daemon, then quit with it playing
#   handoff_switch  like handoff_leave, but switching the daemon from quitting
#                   with us to staying just before quitting
#   handoff_return  pick up the daemon left playing, then stop it
#   failures        songs that won't open, with skip_failed on and off
#   latency         changing the output buffer mid-song
#   stream_buffer   a network stream that stalls, with and without read-ahead
#                   (needs tests/stream_server.py, run_tests.sh starts it)
#   alsa            the real ALSA backend, at volume 0
#
# Prints a line per step and "FAIL: ..." for each failed check; the exit code
# is the number of failures. Everything except alsa uses the null backend, so
# nothing comes out of the speakers.

extends SceneTree

var p
var audio := ""
var t0 := 0
var failures := 0

func say(s):
	print("[%5.2f] %s" % [(Time.get_ticks_msec() - t0) / 1000.0, s])

func check(ok: bool, label: String):
	if not ok:
		failures += 1
		print("FAIL: " + label)

func mag_db(lo: float, hi: float) -> float:
	return linear_to_db(max(p.get_magnitude_for_frequency_range(lo, hi).length(), 1e-9))

func wave_peak() -> float:
	var peak := 0.0
	for f in p.get_waveform(2048):
		peak = max(peak, abs(f.x))
	return peak

func report(label):
	say("%-26s state=%d idx=%d pos=%5.2f dur=%5.2f vol=%.2f eq=%s | 1k=%6.1f 440=%6.1f 3-4k=%6.1f dB | wave=%.3f" % [
		label, p.get_state(), p.get_current_index(), p.get_playback_position(), p.get_duration(), p.volume, p.eq_enabled,
		mag_db(900, 1100), mag_db(400, 480), mag_db(3000, 4000), wave_peak()])

func wait(s):
	await create_timer(s).timeout

func song(name: String) -> String:
	return audio.path_join(name)

func finish():
	say("%d failed checks" % failures)
	quit(failures)

func _initialize():
	t0 = Time.get_ticks_msec()
	var args := OS.get_cmdline_user_args()
	var mode: String = args[0]
	audio = args[1]
	p = ClassDB.instantiate("SongoPlayerHarness")
	p.transport = 2 if mode == "daemon" or mode.begins_with("handoff") else 1
	p.output_backend = "" if mode == "alsa" else "null"
	p.socket_path = args[2]
	p.daemon_exits_with_app = mode == "daemon" or mode == "handoff_switch" or not mode.begins_with("handoff")
	p.finished.connect(func(): say("signal finished"))
	p.error.connect(func(m): say("signal error: " + m))
	p.track_changed.connect(func(i, path): say("signal track_changed %d %s" % [i, path.get_file()]))
	p.state_changed.connect(func(s): say("signal state_changed %d" % s))
	p.queue_changed.connect(func(): say("signal queue_changed %d songs" % p.get_queue().size()))
	p.engine_connection_changed.connect(func(c): say("signal engine_connection_changed %s" % c))
	root.add_child(p)
	match mode:
		"inproc", "daemon": playback()
		"compare": compare()
		"blend": blend()
		"failures": failures_mode()
		"latency": latency_mode()
		"stream_buffer": stream_buffer_mode()
		"handoff_leave": handoff_leave()
		"handoff_switch": handoff_switch()
		"handoff_return": handoff_return()
		"alsa": alsa()

func playback():
	p.loop_queue = true
	p.set_queue(PackedStringArray([song("tone_1000.wav"), song("tone_440.wav")]), 0, 0.0, true)
	await wait(1.0); report("playing 1k")
	check(p.get_state() == 2 and abs(p.get_playback_position() - 1.0) < 0.15, "playing about 1s in")
	check(mag_db(900, 1100) > -15.0 and mag_db(400, 480) < -60.0, "1kHz peak in the spectrum")

	p.seek(2.0); await wait(0.4); report("after seek(2.0)")
	check(abs(p.get_playback_position() - 2.4) < 0.15, "seek moved the position")

	p.pause(); await wait(0.5); report("paused")
	var paused_at: float = p.get_playback_position()
	await wait(0.5); report("still paused")
	check(p.get_state() == 3 and p.get_playback_position() == paused_at, "position holds while paused")
	check(mag_db(900, 1100) < -100.0, "no spectrum while paused")

	p.resume(); await wait(0.4); report("resumed")
	await wait(0.8); report("ran into 440 (natural)")
	check(p.get_current_index() == 1 and mag_db(400, 480) > -15.0, "moved on to the next song by itself")

	p.next(); await wait(0.6); report("next() wraps to 1k")
	check(p.get_current_index() == 0, "next() wrapped around the queue")

	p.volume = 0.5; await wait(0.5); report("volume 0.5")
	check(abs(wave_peak() - 0.25) < 0.02, "waveform follows the volume")

	p.eq_band_gains = PackedFloat32Array([0, 0, 0, 0, 0, -12, 0, 0, 0, 0]); p.eq_enabled = true
	await wait(0.4); report("eq on (analysis is pre-EQ)")
	check(p.eq_enabled and p.eq_band_gains[5] == -12.0, "EQ settings reached the engine")
	p.eq_auto_preamp = false; p.eq_output_gain_db = -3.0
	await wait(0.4); report("eq output: auto preamp off, -3 dB")
	check(not p.eq_auto_preamp and p.eq_output_gain_db == -3.0, "EQ output settings reached the engine")
	p.eq_auto_preamp = true; p.eq_output_gain_db = 0.0

	p.previous(); await wait(0.5); report("previous()")
	check(p.get_current_index() == 1, "previous() early in a song goes back one (wrapping)")

	p.stop(); await wait(0.3); report("stopped")
	check(p.get_state() == 0, "stopped")
	finish()

# Our analyzer next to Godot's own, on the same file
func compare():
	var bus := AudioServer.bus_count
	AudioServer.add_bus(bus)
	AudioServer.set_bus_name(bus, "Viz")
	AudioServer.add_bus_effect(bus, AudioEffectSpectrumAnalyzer.new(), 0)
	var godot_player := AudioStreamPlayer.new()
	godot_player.stream = load("res://audio/tone_1000.wav")
	godot_player.bus = "Viz"
	root.add_child(godot_player)
	await wait(0.2)
	var analyzer: AudioEffectSpectrumAnalyzerInstance = AudioServer.get_bus_effect_instance(bus, 0)
	godot_player.play()
	p.play(song("tone_1000.wav"))
	await wait(1.0)
	var bands := [[20, 200], [400, 480], [900, 1100], [990, 1010], [1500, 2500], [3000, 4000], [8000, 16000]]
	for round in 3:
		var line := ""
		for b in bands:
			for mode in [1, 0]:
				var ours: float = linear_to_db(max(p.get_magnitude_for_frequency_range(b[0], b[1], mode).length(), 1e-9))
				var theirs: float = linear_to_db(max(analyzer.get_magnitude_for_frequency_range(b[0], b[1], mode).length(), 1e-9))
				if mode == 1:
					line += "  %d-%d: max %.1f/%.1f" % [b[0], b[1], ours, theirs]
				else:
					line += " avg %.1f/%.1f" % [ours, theirs]
				# The themes map -60dB..0dB, that's the range that has to match
				if theirs > -60.0:
					check(abs(ours - theirs) < 1.0, "spectrum matches Godot's in %d-%dHz (%.1f vs %.1f dB)" % [b[0], b[1], ours, theirs])
		say("ours/godot dB" + line)
		await wait(0.37)
	godot_player.queue_free()
	finish()

func blend():
	p.blend_time = 1.0
	# Starts 1.5s into a 3s song, so the fade runs from 0.5s to 1.5s from now
	p.set_queue(PackedStringArray([song("tone_1000.wav"), song("tone_440.wav")]), 0, 1.5, true)
	for i in 14:
		await wait(0.15)
		report("blend t+%.2f" % ((i + 1) * 0.15))
		if i == 6:
			check(mag_db(900, 1100) > -25.0 and mag_db(400, 480) > -25.0, "both songs heard mid blend")
	check(p.get_current_index() == 1, "the incoming song took over")
	finish()

func handoff_leave():
	p.loop_queue = true
	p.volume = 0.7
	p.set_queue(PackedStringArray([song("tone_1000.wav"), song("tone_440.wav")]), 1, 1.0, true)
	await wait(0.5); report("leaving while playing")
	finish()

# Started with the daemon going when we do, then told to stay just before
# quitting, as Songo's Quit with Music does
func handoff_switch():
	p.loop_queue = true
	p.volume = 0.7
	p.set_queue(PackedStringArray([song("tone_1000.wav"), song("tone_440.wav")]), 1, 1.0, true)
	await wait(0.5); report("leaving, told to stay")
	p.daemon_exits_with_app = false
	# run_tests.sh checks this ran once the daemon finally goes
	p.daemon_exit_hook = "touch '%s.hook_ran'" % p.socket_path
	finish()

func handoff_return():
	await wait(0.3)
	say("returned: connected=%s queue=%d songs idx=%d path=%s" % [p.is_engine_connected(), p.get_queue().size(), p.get_current_index(), p.get_current_path().get_file()])
	report("picked up")
	check(p.is_engine_connected() and p.get_queue().size() == 2 and p.get_state() == 2, "picked up the daemon left playing")
	check(abs(p.volume - 0.7) < 0.001, "kept the volume set before leaving")
	p.stop(); await wait(0.2)
	finish()

func alsa():
	p.volume = 0.0
	p.set_queue(PackedStringArray([song("tone_1000.wav"), song("tone_5000_48k.wav")]), 0, 0.0, true)
	await wait(0.6); say("alsa playing    state=%d pos=%.3f" % [p.get_state(), p.get_playback_position()])
	check(p.get_state() == 2, "playing through ALSA")
	p.pause(); await wait(0.3)
	var paused_at: float = p.get_playback_position()
	await wait(0.4); say("alsa paused     state=%d pos=%.3f" % [p.get_state(), p.get_playback_position()])
	check(p.get_playback_position() == paused_at, "position holds while paused")
	p.resume(); await wait(0.5); say("alsa resumed    state=%d pos=%.3f" % [p.get_state(), p.get_playback_position()])
	p.seek(2.8); await wait(0.6); say("alsa 48k song   state=%d idx=%d pos=%.3f" % [p.get_state(), p.get_current_index(), p.get_playback_position()])
	check(p.get_current_index() == 1, "moved on to the 48kHz song")
	p.stop(); await wait(0.2); say("alsa stopped    state=%d" % p.get_state())
	finish()

func failures_mode():
	var errors := [0]
	p.error.connect(func(_m): errors[0] += 1)
	p.skip_failed = false
	p.set_queue(PackedStringArray([song("missing.wav"), song("tone_440.wav")]), 0, 0.0, true)
	await wait(0.4); report("skip_failed off")
	check(p.get_state() == 0 and errors[0] == 1, "stopped with one error instead of skipping")
	p.skip_failed = true
	p.play_index(0)
	await wait(0.4); report("skip_failed on")
	check(p.get_state() == 2 and p.get_current_index() == 1 and errors[0] == 2, "skipped to the song that opens")
	p.stop(); await wait(0.2)
	finish()

func latency_mode():
	p.set_queue(PackedStringArray([song("tone_1000.wav")]), 0, 0.0, true)
	await wait(1.0)
	var before: float = p.get_playback_position()
	p.output_latency_ms = 500
	await wait(0.5); report("latency 500ms")
	check(p.get_state() == 2 and abs(p.get_playback_position() - (before + 0.5)) < 0.25, "kept playing from the same spot at 500ms")
	p.output_latency_ms = 100
	await wait(0.5); report("latency 100ms")
	check(p.get_state() == 2 and mag_db(900, 1100) > -15.0, "playing and heard at 100ms")
	p.stop(); await wait(0.2)
	finish()

# How far the song has fallen behind the clock since it started playing
func _stream_lag(buffer_sec: float, url: String) -> float:
	p.stream_buffer_length = buffer_sec
	p.play(url)
	var started := -1
	while started < 0:
		await wait(0.02)
		if p.get_state() == 2:
			started = Time.get_ticks_msec()
	await wait(4.5)
	var lag: float = (Time.get_ticks_msec() - started) / 1000.0 - p.get_playback_position()
	report("read-ahead %.1fs, %.2fs behind" % [buffer_sec, lag])
	p.stop(); await wait(0.3)
	return lag

func stream_buffer_mode():
	var url: String = OS.get_cmdline_user_args()[3]
	var lag := await _stream_lag(1.0, url + "?stall=0.8")
	check(lag < 0.2, "a 1s read-ahead rode out a 0.8s stall (%.2fs behind)" % lag)

	# A stall longer than everything buffered: silence, not a hang
	p.stream_buffer_length = 1.0
	p.play(url + "?stall=20")
	while p.get_state() != 2:
		await wait(0.02)
	await wait(12.0); report("long stall, buffers drained")
	check(p.get_state() == 2, "still playing (silence) through a long stall")
	var asked := Time.get_ticks_msec()
	p.stop()
	while p.get_state() != 0:
		await wait(0.01)
	var took: float = (Time.get_ticks_msec() - asked) / 1000.0
	say("stop took %.2fs mid stall" % took)
	check(took < 0.5, "stop is prompt while the stream is stalled (%.2fs)" % took)
	finish()
