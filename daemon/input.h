// Handheld controls for when Songo isn't open, read straight from /dev/input
// without grabbing, so the frontend and games still see every press:
//   hold Select 1s        quit
//   Select+Right/Left     next / previous (or restart, and both restart
//                         while repeating one)
//   Select+Up/Down        music volume
// Headphones being unplugged pauses, whether or not Songo is open, on devices
// whose kernel reports the jack as an input switch (SW_HEADPHONE_INSERT).
// Headset media buttons work the same way: play/pause, next and previous.

#ifndef SONGO_DAEMON_INPUT_H
#define SONGO_DAEMON_INPUT_H

#include <vector>

namespace songo {

enum class MediaPress {
	NONE,
	TOGGLE,
	PLAY,
	PAUSE,
};

struct InputActions {
	// -1 previous/restart, 1 next, 0 none
	int skip = 0;
	// Net Select+Up (+1) / Select+Down (-1) presses
	int volume_steps = 0;
	bool quit = false;
	bool headphones_unplugged = false;
	// The last headset play/pause button pressed
	MediaPress media = MediaPress::NONE;
};

class Input {
public:
	~Input();

	// Opens every event device we're allowed to read. Fine if that's none
	// (desktops, where only the input group can).
	void open_devices();
	void close_devices();

	// For poll()
	std::vector<int> fds() const;

	// Reads what's pending. With `active` off (Songo is open and handling
	// presses itself) events are read and dropped, so they don't pile up.
	// Headphone unplugs and headset buttons are reported either way.
	InputActions poll(bool active);

private:
	struct Device {
		int fd;
		// Codes this device sends for Select
		const std::vector<int> *select_codes;
		// Reports a headphone jack, so its BTN_0 is the headset button
		bool jack;
	};

	std::vector<Device> devices;
	double select_pressed_at = -1.0;
	// Set once a Select combo fires, so finishing the combo with Select
	// still held doesn't also count as the quit hold
	bool select_combo_used = false;
};

} // namespace songo

#endif // SONGO_DAEMON_INPUT_H
