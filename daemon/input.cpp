#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace songo {

namespace {

// BTN_SELECT on most gamepads, KEY_RIGHTCTRL on Miyoo-style keyboard mappings
const std::vector<int> DEFAULT_SELECT_CODES = { BTN_SELECT, KEY_RIGHTCTRL };
// Anbernic's H700 pads (RG34XX, RG35XX Plus/H/SP, RG40XX, ...) send Select as
// BTN_TL, and BTN_SELECT is their L2
const std::vector<int> ANBERNIC_SELECT_CODES = { BTN_TL };
const double SELECT_HOLD_SEC = 1.0;

// Whether the device has the headphone jack switch. Codecs that detect headset
// buttons but have no keys set for them report the first one as BTN_0 there,
// and only there, as on a gamepad BTN_0 could be anything.
bool is_jack(int fd) {
	unsigned long bits[(SW_MAX + 1 + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long))] = {};
	if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(bits)), bits) < 0) {
		return false;
	}
	const size_t word_bits = 8 * sizeof(unsigned long);
	return (bits[SW_HEADPHONE_INSERT / word_bits] >> (SW_HEADPHONE_INSERT % word_bits)) & 1;
}

// Headset (or any media remote) button, on its first press only, not repeats
MediaPress media_press(const input_event &ev, bool jack) {
	if (ev.type != EV_KEY || ev.value != 1) {
		return MediaPress::NONE;
	}
	switch (ev.code) {
		case KEY_PLAYPAUSE:
		case KEY_MEDIA:
			return MediaPress::TOGGLE;
		case KEY_PLAYCD:
			return MediaPress::PLAY;
		case KEY_PAUSECD:
			return MediaPress::PAUSE;
		case BTN_0:
			return jack ? MediaPress::TOGGLE : MediaPress::NONE;
		default:
			return MediaPress::NONE;
	}
}

double now_sec() {
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

bool starts_with(const char *s, const char *prefix) {
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Picks the Select codes for a device from its evdev name. The H700 names are
// the ones Knulli's es_input.cfg maps Select to BTN_TL for.
const std::vector<int> *select_codes_for(int fd) {
	char name[256] = {};
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
		return &DEFAULT_SELECT_CODES;
	}
	if (starts_with(name, "Anbernic ") || strcmp(name, "ANBERNIC-keys") == 0 ||
			strcmp(name, "Deeplay-keys") == 0) {
		fprintf(stderr, "Using BTN_TL as Select for \"%s\"\n", name);
		return &ANBERNIC_SELECT_CODES;
	}
	return &DEFAULT_SELECT_CODES;
}

// D-pad press along one axis: -1 for the negative direction (left/up), 1 for
// the positive one (right/down), 0 for anything else. Depending on the device
// the d-pad is gamepad buttons, keyboard arrows or a hat axis.
int dpad_press(const input_event &ev, int btn_neg, int btn_pos, int key_neg, int key_pos, int hat_axis) {
	if (ev.type == EV_KEY && ev.value == 1) {
		if (ev.code == btn_neg || ev.code == key_neg) {
			return -1;
		}
		if (ev.code == btn_pos || ev.code == key_pos) {
			return 1;
		}
	} else if (ev.type == EV_ABS && ev.code == hat_axis) {
		return ev.value < 0 ? -1 : ev.value > 0 ? 1 : 0;
	}
	return 0;
}

} // namespace

Input::~Input() {
	close_devices();
}

void Input::open_devices() {
	DIR *dir = opendir("/dev/input");
	if (!dir) {
		return;
	}
	while (dirent *entry = readdir(dir)) {
		if (strncmp(entry->d_name, "event", 5) != 0) {
			continue;
		}
		std::string path = std::string("/dev/input/") + entry->d_name;
		// Read only, no grab, so the frontend and games still see every press
		int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0) {
			devices.push_back({ fd, select_codes_for(fd), is_jack(fd) });
		}
	}
	closedir(dir);
	fprintf(stderr, "Watching %zu input devices\n", devices.size());
}

void Input::close_devices() {
	for (const Device &device : devices) {
		close(device.fd);
	}
	devices.clear();
}

std::vector<int> Input::fds() const {
	std::vector<int> out;
	for (const Device &device : devices) {
		out.push_back(device.fd);
	}
	return out;
}

InputActions Input::poll(bool active) {
	InputActions actions;
	input_event ev;
	for (const Device &device : devices) {
		while (read(device.fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT && ev.value == 0) {
				actions.headphones_unplugged = true;
				continue;
			}
			MediaPress media = media_press(ev, device.jack);
			if (media != MediaPress::NONE) {
				actions.media = media;
				continue;
			}
			if (ev.type == EV_KEY && ev.value == 1 && (ev.code == KEY_NEXTSONG || ev.code == KEY_PREVIOUSSONG)) {
				actions.skip = ev.code == KEY_NEXTSONG ? 1 : -1;
				continue;
			}
			if (!active) {
				continue;
			}
			bool is_select = false;
			for (int code : *device.select_codes) {
				is_select = is_select || (ev.type == EV_KEY && ev.code == code);
			}
			if (is_select) {
				if (ev.value == 1) {
					select_pressed_at = now_sec();
					select_combo_used = false;
				} else if (ev.value == 0) {
					select_pressed_at = -1.0;
				}
				continue;
			}
			if (select_pressed_at < 0.0) {
				continue;
			}
			int x = dpad_press(ev, BTN_DPAD_LEFT, BTN_DPAD_RIGHT, KEY_LEFT, KEY_RIGHT, ABS_HAT0X);
			int y = dpad_press(ev, BTN_DPAD_UP, BTN_DPAD_DOWN, KEY_UP, KEY_DOWN, ABS_HAT0Y);
			if (x != 0) {
				actions.skip = x;
				select_combo_used = true;
			}
			if (y != 0) {
				// Up is negative on the d-pad, but raises the volume
				actions.volume_steps -= y;
				select_combo_used = true;
			}
		}
	}
	if (!active) {
		// Whatever was held while Songo had the controls doesn't count
		select_pressed_at = -1.0;
		return actions;
	}
	if (select_pressed_at >= 0.0 && !select_combo_used && now_sec() - select_pressed_at >= SELECT_HOLD_SEC) {
		fprintf(stderr, "Select held, quitting\n");
		actions.quit = true;
		select_pressed_at = -1.0;
	}
	return actions;
}

} // namespace songo
