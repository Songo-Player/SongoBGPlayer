// songo_bgplayer: hosts the engine outside Songo, so music keeps playing after
// Songo exits.
//
//   songo_bgplayer [playlist.m3u] [options]
//
// Songo controls it over a Unix socket (see common/protocol.h) and reads its
// live state from the shared memory it hands over on connect. While nobody is
// connected, the handheld controls in input.h work instead.
//
// Started with a playlist it plays that straight away, which is how Songo
// hands its queue over on exit. If a daemon is already running, the playlist
// goes to that one instead. Besides the paths, the playlist can carry Songo's
// playback state as comment lines, which other m3u readers ignore:
//   #SONGO-START-INDEX:3      queue index to start at
//   #SONGO-START-POSITION:42  seconds into that song
//   #SONGO-REPEAT-ONE:1       repeat the current song instead of moving on
//   #SONGO-VOLUME:0.8         linear gain, Songo's music volume
//   #SONGO-EQ:0,-3,2,...      Songo's 10 EQ band gains in dB, only when its EQ is on
//   #SONGO-LOOP:0             stop at the end of the queue rather than looping
//
// Options:
//   --blend SECONDS      crossfade between songs like Songo's playback blend
//   --idle               start without a playlist and wait for Songo
//   --socket PATH        listen here rather than the default socket
//   --backend NAME       alsa (default) or null (no audio, for testing)
//   --device NAME        ALSA device, rather than "default"
//   --no-input           ignore the handheld controls
//   --exit-on-disconnect quit as soon as the last client disconnects (Songo
//                        can switch this with the exit_on_disconnect command)
//   --log FILE           append messages here rather than to stderr
//
// It quits when Select is held for a second, on SIGTERM/SIGINT, on a "quit"
// command, or when nothing is playing and nobody has been connected for a
// few seconds. Its PID is in the lock file next to the socket, for launchers
// that need to stop it. Songo can leave it an exit_hook command to run as it
// goes, eg: undoing the CFW tweaks it made for music playing in the background.

#include "input.h"
#include "protocol.h"
#include "songo_engine.h"
#include "songo_shared.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace songo;

namespace {

const float VOLUME_STEP = 0.1f;
// Quit once nothing has played and nobody has been connected for this long
const double IDLE_EXIT_SEC = 3.0;
// Longer when just started with --idle, so whoever started us can connect
const double STARTUP_IDLE_EXIT_SEC = 10.0;
// A client this far behind on reading events is stuck, drop it
const size_t MAX_CLIENT_BACKLOG = 16 * 1024 * 1024;

volatile sig_atomic_t quit_requested = 0;

void on_signal(int) {
	quit_requested = 1;
}

double now_sec() {
	return (double)monotonic_ns() / 1e9;
}

struct Options {
	std::string playlist;
	double blend = -1.0;
	bool idle = false;
	std::string socket_path;
	std::string backend;
	std::string device;
	bool input = true;
	bool exit_on_disconnect = false;
	std::string log;
};

struct Playlist {
	std::vector<std::string> paths;
	int start_index = 0;
	double start_position = 0.0;
	bool repeat_one = false;
	bool loop = true;
	float volume = 1.0f;
	bool eq_enabled = false;
	float eq_gains_db[EQ_BANDS] = {};
};

struct Client {
	int fd = -1;
	SongoSubscription *subscription = nullptr;
	std::string in;
	std::string out;
	bool closing = false;
};

// Songo launches us with fork/exec, so we inherit whatever it had open that
// wasn't close-on-exec. Close it all so we never keep Songo's files, sockets or
// devices open after it exits. stdio is left alone, the start script sets it.
void close_inherited_fds() {
	DIR *dir = opendir("/proc/self/fd");
	if (!dir) {
		return;
	}
	std::vector<int> fds;
	while (dirent *entry = readdir(dir)) {
		int fd = atoi(entry->d_name);
		if (fd > 2 && fd != dirfd(dir)) {
			fds.push_back(fd);
		}
	}
	closedir(dir);
	for (int fd : fds) {
		close(fd);
	}
}

std::string trim(const std::string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

bool read_playlist(const std::string &path, Playlist &r_playlist) {
	std::ifstream file(path);
	if (!file) {
		fprintf(stderr, "Can't open playlist %s\n", path.c_str());
		return false;
	}

	// Absolute, since the playlist may be handed to a daemon running elsewhere
	size_t slash = path.find_last_of('/');
	std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
	if (char *resolved = realpath(dir.c_str(), nullptr)) {
		dir = resolved;
		free(resolved);
	}

	std::string line;
	while (std::getline(file, line)) {
		line = trim(line);
		if (line.empty()) {
			continue;
		}
		if (line[0] == '#') {
			size_t colon = line.find(':');
			if (colon == std::string::npos) {
				continue;
			}
			std::string key = line.substr(0, colon);
			const char *value = line.c_str() + colon + 1;
			if (key == "#SONGO-START-INDEX") {
				r_playlist.start_index = atoi(value);
			} else if (key == "#SONGO-START-POSITION") {
				r_playlist.start_position = atof(value);
			} else if (key == "#SONGO-REPEAT-ONE") {
				r_playlist.repeat_one = atoi(value) != 0;
			} else if (key == "#SONGO-LOOP") {
				r_playlist.loop = atoi(value) != 0;
			} else if (key == "#SONGO-EQ") {
				// Comma separated dB per band, missing bands stay flat
				r_playlist.eq_enabled = true;
				const char *cursor = value;
				for (int band = 0; band < EQ_BANDS && *cursor; band++) {
					char *end = nullptr;
					r_playlist.eq_gains_db[band] = strtof(cursor, &end);
					if (end == cursor) {
						break;
					}
					cursor = *end == ',' ? end + 1 : end;
				}
			} else if (key == "#SONGO-VOLUME") {
				r_playlist.volume = std::clamp((float)atof(value), 0.0f, 1.0f);
			}
			continue;
		}
		// Relative entries are relative to the playlist, URLs and absolute paths as is
		if (line[0] != '/' && line.find("://") == std::string::npos) {
			line = dir + "/" + line;
		}
		r_playlist.paths.push_back(line);
	}

	if (r_playlist.paths.empty()) {
		fprintf(stderr, "Playlist %s has no entries\n", path.c_str());
		return false;
	}
	if (r_playlist.start_index < 0 || r_playlist.start_index >= (int)r_playlist.paths.size()) {
		r_playlist.start_index = 0;
		r_playlist.start_position = 0.0;
	}
	return true;
}

// The commands that set a daemon up to play `playlist`
std::vector<Message> playlist_commands(const Playlist &playlist, double blend) {
	std::vector<Message> out;
	out.push_back(Message("volume").add((double)playlist.volume));
	Message eq("eq");
	eq.add(playlist.eq_enabled ? 1 : 0);
	for (int band = 0; band < EQ_BANDS; band++) {
		eq.add((double)playlist.eq_gains_db[band]);
	}
	out.push_back(eq);
	if (blend >= 0.0) {
		out.push_back(Message("blend").add(blend));
	}
	out.push_back(Message("repeat_one").add(playlist.repeat_one ? 1 : 0));
	out.push_back(Message("loop_queue").add(playlist.loop ? 1 : 0));
	Message queue("set_queue");
	queue.add(playlist.start_index).add(playlist.start_position).add(1);
	for (const std::string &path : playlist.paths) {
		queue.add(path);
	}
	out.push_back(queue);
	return out;
}

int connect_socket(const std::string &path) {
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr.sun_path)) {
		return -1;
	}
	memcpy(addr.sun_path, path.c_str(), path.size());
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool write_all(int fd, const std::string &data) {
	size_t done = 0;
	while (done < data.size()) {
		ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		done += (size_t)n;
	}
	return true;
}

// Another daemon is running: hand it the playlist and get out of the way
int forward_to_running_daemon(const Options &options, const Playlist *playlist) {
	if (!playlist) {
		fprintf(stderr, "A player is already running on %s\n", options.socket_path.c_str());
		return 0;
	}
	// It might still be starting up and not listening yet
	int fd = -1;
	double give_up_at = now_sec() + 3.0;
	while ((fd = connect_socket(options.socket_path)) < 0 && now_sec() < give_up_at && !quit_requested) {
		usleep(50000);
	}
	if (fd < 0) {
		fprintf(stderr, "A player is running but not answering on %s\n", options.socket_path.c_str());
		return 1;
	}
	std::string data;
	for (const Message &message : playlist_commands(*playlist, options.blend)) {
		data += encode_line(message);
	}
	bool ok = write_all(fd, data);
	close(fd);
	fprintf(stderr, ok ? "Handed the playlist to the running player\n" : "Couldn't hand the playlist over\n");
	return ok ? 0 : 1;
}

int listen_socket(const std::string &path) {
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof(addr.sun_path)) {
		fprintf(stderr, "Socket path too long: %s\n", path.c_str());
		return -1;
	}
	memcpy(addr.sun_path, path.c_str(), path.size());
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	// We hold the lock, so anything already there is left over from a crash
	unlink(path.c_str());
	mode_t old_umask = umask(0077);
	int bound = bind(fd, (sockaddr *)&addr, sizeof(addr));
	umask(old_umask);
	if (bound < 0 || listen(fd, 8) < 0) {
		fprintf(stderr, "Can't listen on %s: %s\n", path.c_str(), strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

// Sends the hello, with the shared memory's fd riding along with it
bool send_hello(int fd, int shared_fd, size_t shared_size) {
	std::string line = encode_line(Message("hello").add(PROTOCOL_VERSION).add((double)shared_size));
	iovec iov = { (void *)line.data(), line.size() };
	msghdr msg = {};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	if (shared_fd >= 0) {
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &shared_fd, sizeof(int));
	}
	return sendmsg(fd, &msg, MSG_NOSIGNAL) == (ssize_t)line.size();
}

void flush_client(Client &client) {
	while (!client.out.empty()) {
		ssize_t n = send(client.fd, client.out.data(), client.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) {
			client.out.erase(0, (size_t)n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			break;
		}
		client.closing = true;
		return;
	}
	if (client.out.size() > MAX_CLIENT_BACKLOG) {
		fprintf(stderr, "Client stopped reading, dropping it\n");
		client.closing = true;
	}
}

void print_usage(const char *argv0) {
	fprintf(stderr,
			"Usage: %s [playlist.m3u] [--blend SECONDS] [--idle] [--socket PATH]\n"
			"       [--backend alsa|null] [--device NAME] [--no-input] [--exit-on-disconnect] [--log FILE]\n",
			argv0);
}

bool parse_args(int argc, char **argv, Options &r_options) {
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		bool has_value = i + 1 < argc;
		if (arg == "--blend" && has_value) {
			r_options.blend = std::max(0.0, atof(argv[++i]));
		} else if (arg == "--idle") {
			r_options.idle = true;
		} else if (arg == "--socket" && has_value) {
			r_options.socket_path = argv[++i];
		} else if (arg == "--backend" && has_value) {
			r_options.backend = argv[++i];
		} else if (arg == "--device" && has_value) {
			r_options.device = argv[++i];
		} else if (arg == "--no-input") {
			r_options.input = false;
		} else if (arg == "--exit-on-disconnect") {
			r_options.exit_on_disconnect = true;
		} else if (arg == "--log" && has_value) {
			r_options.log = argv[++i];
		} else if (arg.rfind("--", 0) != 0 && r_options.playlist.empty()) {
			r_options.playlist = arg;
		} else {
			return false;
		}
	}
	return r_options.idle || !r_options.playlist.empty();
}

} // namespace

int main(int argc, char **argv) {
	close_inherited_fds();

	Options options;
	if (!parse_args(argc, argv, options)) {
		print_usage(argv[0]);
		return 1;
	}
	if (!options.log.empty() && freopen(options.log.c_str(), "a", stderr)) {
		setvbuf(stderr, nullptr, _IOLBF, 0);
	}
	if (options.socket_path.empty()) {
		options.socket_path = default_socket_path();
	}

	struct sigaction sa = {};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, nullptr);
	sigaction(SIGINT, &sa, nullptr);
	signal(SIGHUP, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);

	std::unique_ptr<Playlist> playlist;
	if (!options.playlist.empty()) {
		playlist = std::make_unique<Playlist>();
		if (!read_playlist(options.playlist, *playlist)) {
			return 1;
		}
	}

	// One daemon per socket. The lock outlives crashes, unlike the socket file.
	std::string lock_path = options.socket_path + ".lock";
	int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lock_fd < 0) {
		fprintf(stderr, "Can't open %s: %s\n", lock_path.c_str(), strerror(errno));
		return 1;
	}
	if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
		return forward_to_running_daemon(options, playlist.get());
	}
	std::string pid = std::to_string(getpid()) + "\n";
	if (ftruncate(lock_fd, 0) != 0 || pwrite(lock_fd, pid.data(), pid.size(), 0) != (ssize_t)pid.size()) {
		fprintf(stderr, "Can't write the PID to %s\n", lock_path.c_str());
	}

	int listen_fd = listen_socket(options.socket_path);
	if (listen_fd < 0) {
		return 1;
	}

	SongoEngineConfig config = {};
	config.backend = options.backend.empty() ? nullptr : options.backend.c_str();
	config.device = options.device.empty() ? nullptr : options.device.c_str();
	SongoEngine *engine = songo_engine_create(&config);
	if (!engine) {
		unlink(options.socket_path.c_str());
		return 1;
	}
	size_t shared_size = 0;
	const Shared *shared = static_cast<const Shared *>(songo_engine_shared(engine, &shared_size));
	int shared_fd = songo_engine_shared_fd(engine);

	if (playlist) {
		fprintf(stderr, "Playing %zu songs from index %d at %.1fs, volume %.2f, blend %.1fs%s, EQ %s\n",
				playlist->paths.size(), playlist->start_index, playlist->start_position,
				playlist->volume, std::max(options.blend, 0.0), playlist->repeat_one ? ", repeating one" : "",
				playlist->eq_enabled ? "on" : "off");
		for (const Message &message : playlist_commands(*playlist, options.blend)) {
			dispatch_command(engine, message);
		}
	}
	fprintf(stderr, "Listening on %s\n", options.socket_path.c_str());

	Input input;
	if (options.input) {
		input.open_devices();
	}

	std::vector<std::unique_ptr<Client>> clients;
	std::string exit_hook;
	bool had_client = false;
	double idle_since = -1.0;
	bool quit = false;

	while (!quit && !quit_requested) {
		std::vector<pollfd> fds;
		fds.push_back({ listen_fd, POLLIN, 0 });
		for (const auto &client : clients) {
			fds.push_back({ client->fd, (short)(POLLIN | (client->out.empty() ? 0 : POLLOUT)), 0 });
			fds.push_back({ songo_subscription_fd(client->subscription), POLLIN, 0 });
		}
		for (int fd : input.fds()) {
			fds.push_back({ fd, POLLIN, 0 });
		}
		// The timeout is what catches a Select hold, which has no event of its own
		poll(fds.data(), fds.size(), 50);
		if (quit_requested) {
			break;
		}

		while (true) {
			int fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (fd < 0) {
				break;
			}
			auto client = std::make_unique<Client>();
			client->fd = fd;
			// A playlist handed over by another songo_bgplayer arrives from a
			// client that's already gone, so the hello can fail. What it sent
			// is still waiting to be read.
			client->closing = !send_hello(fd, shared_fd, shared_size);
			client->subscription = songo_engine_subscribe(engine);
			clients.push_back(std::move(client));
			had_client = true;
			fprintf(stderr, "Songo connected (%zu connected)\n", clients.size());
		}

		for (auto &client : clients) {
			char buf[65536];
			while (true) {
				ssize_t n = recv(client->fd, buf, sizeof(buf), MSG_DONTWAIT);
				if (n > 0) {
					client->in.append(buf, (size_t)n);
					continue;
				}
				if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
					client->closing = true;
				}
				break;
			}
			std::vector<Message> messages;
			take_lines(client->in, messages);
			for (const Message &message : messages) {
				if (message.name() == "quit") {
					fprintf(stderr, "Asked to quit\n");
					quit = true;
				} else if (message.name() == "exit_on_disconnect") {
					options.exit_on_disconnect = message.arg_int(0) != 0;
				} else if (message.name() == "exit_hook") {
					exit_hook = message.arg(0);
				} else if (!dispatch_command(engine, message)) {
					fprintf(stderr, "Unknown command \"%s\"\n", message.name().c_str());
				}
			}

			SongoEvent event;
			while (!client->closing && songo_subscription_poll(client->subscription, &event)) {
				client->out += encode_line(event_message(event));
			}
			if (!client->closing) {
				flush_client(*client);
			}
		}

		for (auto it = clients.begin(); it != clients.end();) {
			if ((*it)->closing) {
				songo_subscription_close((*it)->subscription);
				close((*it)->fd);
				it = clients.erase(it);
				fprintf(stderr, "Songo disconnected (%zu connected)\n", clients.size());
			} else {
				++it;
			}
		}

		// The handheld controls only while Songo isn't there to handle them.
		// Headset buttons and unplugs come through either way, Songo has no
		// handling of its own for them.
		InputActions actions = input.poll(clients.empty());
		if (actions.quit) {
			quit = true;
		}
		if (actions.headphones_unplugged) {
			Status status;
			if (read_status(shared, status) &&
					(status.state == SONGO_STATE_PLAYING || status.state == SONGO_STATE_LOADING)) {
				fprintf(stderr, "Headphones unplugged, pausing\n");
				songo_engine_pause(engine);
			}
		}
		if (actions.media != MediaPress::NONE) {
			Status status;
			bool playing = read_status(shared, status) &&
					(status.state == SONGO_STATE_PLAYING || status.state == SONGO_STATE_LOADING);
			if (playing && actions.media != MediaPress::PLAY) {
				fprintf(stderr, "Headset button, pausing\n");
				songo_engine_pause(engine);
			} else if (!playing && actions.media != MediaPress::PAUSE) {
				fprintf(stderr, "Headset button, playing\n");
				songo_engine_resume(engine);
			}
		}
		if (actions.skip > 0) {
			fprintf(stderr, "Skipping to the next song\n");
			songo_engine_next(engine);
		} else if (actions.skip < 0) {
			songo_engine_previous(engine);
		}
		if (actions.volume_steps != 0) {
			Status status;
			if (read_status(shared, status)) {
				float volume = std::clamp(status.volume + actions.volume_steps * VOLUME_STEP, 0.0f, 1.0f);
				songo_engine_set_volume(engine, volume);
				fprintf(stderr, "Music volume %d%%\n", (int)(volume * 100.0f + 0.5f));
			}
		}

		if (options.exit_on_disconnect && had_client && clients.empty()) {
			fprintf(stderr, "Last client gone, quitting\n");
			break;
		}
		Status status;
		bool busy = !clients.empty() || !read_status(shared, status) ||
				status.state == SONGO_STATE_PLAYING || status.state == SONGO_STATE_LOADING;
		if (busy) {
			idle_since = -1.0;
		} else if (idle_since < 0.0) {
			idle_since = now_sec();
		} else if (now_sec() - idle_since >= (had_client || playlist ? IDLE_EXIT_SEC : STARTUP_IDLE_EXIT_SEC)) {
			fprintf(stderr, "Nothing playing and nobody connected, quitting\n");
			break;
		}
	}

	for (auto &client : clients) {
		songo_subscription_close(client->subscription);
		close(client->fd);
	}
	clients.clear();
	songo_engine_destroy(engine);
	close(listen_fd);
	unlink(options.socket_path.c_str());

	// Waited for, with the lock still held, so whoever's waiting for us to
	// go knows the hook is done once we have
	if (!exit_hook.empty()) {
		fprintf(stderr, "Running exit hook %s\n", exit_hook.c_str());
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/sh", "sh", "-c", exit_hook.c_str(), (char *)nullptr);
			_exit(127);
		}
		if (child > 0) {
			waitpid(child, nullptr, 0);
		}
	}
	close(lock_fd);
	fprintf(stderr, "Exited\n");
	return 0;
}
