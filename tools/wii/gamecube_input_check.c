/* HWI-039: the GameCube pad as the player's controller, checked on the host.
 *
 * The Wii's XInput (port/wii/engine/wii_xinput.c, compiled whole with
 * WII_XINPUT_STANDALONE against the XDK headers) under the engine's own
 * gamepad read: input_get_device_states' gamepad half, its tables, dead zone,
 * hold counts and analog thresholds, taken verbatim from
 * source/input/input_xbox.c by tools/wii/test_gamecube_input.py into
 * engine_input_extract.h, with the profile's default button layout
 * (source/input/input_abstraction.c). The pad source is scripted here and the
 * clock is the engine's system_milliseconds.
 *
 * Exit 0 and "ok <n> checks" when every check holds; each semantic action
 * prints "action <name> ok". */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xdk_xbox.h"

typedef unsigned char byte;
typedef unsigned short word;
typedef byte boolean;

static unsigned long checks;
static unsigned long failures;

#define CHECK(condition, ...) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			fprintf(stderr, "FAIL line %d: ", __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fputc('\n', stderr); \
		} \
	} while (0)

/* ---------- the engine's surroundings */

static unsigned long clock_ms = 1000;
static unsigned long engine_errors;

static unsigned long system_milliseconds(void)
{
	return clock_ms;
}

#define _error_silent 0

static void error(int level, const char *format, ...)
{
	(void)level;
	(void)format;
	engine_errors++;
}

#define csmemset memset
#define match_assert(file, line, condition) CHECK((condition), "engine assert at %s:%d: %s", file, line, #condition)

/* ---------- the Wii's XInput */

#define WII_XINPUT_STANDALONE
#include "wii_xinput.c"

static DWORD last_error;

void WINAPI SetLastError(DWORD code)
{
	last_error = code;
}

/* ---------- the engine's read (generated) */

#include "engine_input_extract.h"

/* ---------- the pads */

static struct wii_pad_state pads[WII_PAD_PORTS];
static unsigned int pads_connected;

static unsigned int scan(void)
{
	return pads_connected;
}

static void read_pad(int port, struct wii_pad_state *state)
{
	*state = pads[port];
}

static void reset(void)
{
	memset(pads, 0, sizeof(pads));
	pads_connected = 0;
	memset(&input_globals, 0, sizeof(input_globals));
	memset(gamepad_button_down_times, 0, sizeof(gamepad_button_down_times));
	XInitDevices(0, NULL);
	wii_xinput_set_pad_source(scan, read_pad);
}

static void plug(int port, BOOL connected)
{
	if (connected)
	{
		pads_connected |= 1u << port;
		pads[port].connected = 1;
	}
	else
	{
		pads_connected &= ~(1u << port);
		memset(&pads[port], 0, sizeof(pads[port]));
	}
}

/* one engine frame: the device changes and the reads, ms after the last */
static void frame(unsigned long ms)
{
	clock_ms += ms;
	engine_get_device_states();
}

static void game(void)
{
	halo_ui_pointer_update(FALSE, NULL);
}

static void menus(void)
{
	halo_ui_pointer_update(TRUE, NULL);
}

static const byte *buttons(int port)
{
	return input_globals.gamepad_states[port].buttons;
}

/* ---------- the layers */

struct layer_case
{
	const char *name;
	unsigned short pad;
	short menu_button; /* the gamepad button in the menus */
	short game_button; /* and in the game */
};

static const struct layer_case layer_cases[] =
{
	{ "A", WII_PAD_A, _gamepad_analog_button_a, _gamepad_analog_button_a },
	{ "B", WII_PAD_B, _gamepad_analog_button_b, _gamepad_analog_button_b },
	{ "X", WII_PAD_X, _gamepad_analog_button_x, _gamepad_analog_button_x },
	{ "Y", WII_PAD_Y, _gamepad_analog_button_y, _gamepad_analog_button_y },
	{ "Start", WII_PAD_START, _gamepad_binary_button_start, _gamepad_binary_button_start },
	{ "Z", WII_PAD_Z, _gamepad_binary_button_back, _gamepad_binary_button_left_thumb },
	{ "up", WII_PAD_UP, _gamepad_binary_button_dpad_up, _gamepad_analog_button_white },
	{ "down", WII_PAD_DOWN, _gamepad_binary_button_dpad_down, _gamepad_binary_button_back },
	{ "left", WII_PAD_LEFT, _gamepad_binary_button_dpad_left, _gamepad_analog_button_black },
	{ "right", WII_PAD_RIGHT, _gamepad_binary_button_dpad_right, _gamepad_binary_button_right_thumb },
};

static void check_only_button(const char *what, short expected)
{
	short button;

	for (button = 0; button < NUMBER_OF_GAMEPAD_BUTTONS; button++)
	{
		if (button == expected)
			CHECK(buttons(0)[button] == 1, "%s: button %d pressed once (%d)", what, button, buttons(0)[button]);
		else
			CHECK(buttons(0)[button] == 0, "%s: button %d not pressed (%d)", what, button, buttons(0)[button]);
	}
}

static void check_layers(void)
{
	size_t index;

	for (index = 0; index < sizeof(layer_cases) / sizeof(layer_cases[0]); index++)
	{
		const struct layer_case *c = &layer_cases[index];
		int pass;

		for (pass = 0; pass < 2; pass++)
		{
			char what[64];

			reset();
			plug(0, TRUE);
			if (pass)
				game();
			frame(33);
			pads[0].buttons = c->pad;
			frame(33);
			snprintf(what, sizeof(what), "%s in the %s", c->name, pass ? "game" : "menus");
			check_only_button(what, pass ? c->game_button : c->menu_button);
			if (c->game_button < NUMBER_OF_GAMEPAD_ANALOG_BUTTONS && pass)
				CHECK(input_globals.gamepad_states[0].analog_buttons[c->game_button] == 0xFF, "%s: fully pressed", what);
			pads[0].buttons = 0;
			frame(33);
			CHECK(buttons(0)[pass ? c->game_button : c->menu_button] == 0, "%s: released", what);
		}
	}

	/* the game's layer has no D-pad (its movement is the main stick's) */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	pads[0].buttons = WII_PAD_UP | WII_PAD_DOWN | WII_PAD_LEFT | WII_PAD_RIGHT;
	frame(33);
	CHECK(!buttons(0)[_gamepad_binary_button_dpad_up] && !buttons(0)[_gamepad_binary_button_dpad_down] &&
		!buttons(0)[_gamepad_binary_button_dpad_left] && !buttons(0)[_gamepad_binary_button_dpad_right],
		"no D-pad in the game");
}

/* ---------- the player's actions (the profile's default layout) */

struct action_case
{
	const char *control;
	unsigned short pad;
	BYTE trigger_l, trigger_r;
};

static const struct action_case action_cases[] =
{
	{ "jump", WII_PAD_A, 0, 0 },
	{ "switch_grenades", WII_PAD_LEFT, 0, 0 },
	{ "action", WII_PAD_X, 0, 0 },
	{ "switch_weapons", WII_PAD_Y, 0, 0 },
	{ "melee", WII_PAD_B, 0, 0 },
	{ "flashlight", WII_PAD_UP, 0, 0 },
	{ "grenade", 0, 200, 0 },
	{ "primary_trigger", 0, 0, 200 },
	{ "start", WII_PAD_START, 0, 0 },
	{ "back", WII_PAD_DOWN, 0, 0 },
	{ "crouch", WII_PAD_Z, 0, 0 },
	{ "zoom", WII_PAD_RIGHT, 0, 0 },
};

static short control_button(const char *control)
{
	size_t index;

	for (index = 0; index < sizeof(default_game_controls) / sizeof(default_game_controls[0]); index++)
	{
		if (!strcmp(default_game_controls[index].name, control))
			return default_game_controls[index].button;
	}
	return NONE_BUTTON;
}

static void check_actions(void)
{
	size_t index;

	CHECK(sizeof(action_cases) / sizeof(action_cases[0]) == sizeof(default_game_controls) / sizeof(default_game_controls[0]),
		"every game control has a case");
	for (index = 0; index < sizeof(action_cases) / sizeof(action_cases[0]); index++)
	{
		const struct action_case *c = &action_cases[index];
		short button = control_button(c->control);
		unsigned long before = failures;
		size_t other;
		int held;

		CHECK(button != NONE_BUTTON, "%s is in the profile's layout", c->control);
		if (button == NONE_BUTTON)
			continue;
		reset();
		plug(0, TRUE);
		game();
		frame(33);
		pads[0].buttons = c->pad;
		pads[0].trigger_l = c->trigger_l;
		pads[0].trigger_r = c->trigger_r;
		frame(33);
		CHECK(buttons(0)[button] == 1, "%s pressed on the read it went down (%d)", c->control, buttons(0)[button]);
		/* no other control of the layout is pressed with it */
		for (other = 0; other < sizeof(default_game_controls) / sizeof(default_game_controls[0]); other++)
		{
			if (default_game_controls[other].button != button)
				CHECK(!buttons(0)[default_game_controls[other].button], "%s presses %s too", c->control,
					default_game_controls[other].name);
		}
		for (held = 0; held < 10; held++)
		{
			frame(33);
			CHECK(buttons(0)[button] >= 2, "%s held, not pressed again (%d)", c->control, buttons(0)[button]);
		}
		pads[0].buttons = 0;
		pads[0].trigger_l = pads[0].trigger_r = 0;
		frame(33);
		CHECK(buttons(0)[button] == 0, "%s released", c->control);
		if (failures == before)
			printf("action %s ok\n", c->control);
	}
}

/* ---------- triggers */

/* press edges of the right trigger (fire) over a series of raw values */
static int fire_presses(const unsigned char *values, size_t count, unsigned long ms)
{
	int presses = 0;
	size_t index;

	for (index = 0; index < count; index++)
	{
		pads[0].trigger_r = values[index];
		frame(ms);
		if (buttons(0)[_gamepad_analog_button_right_trigger] == 1)
			presses++;
	}
	return presses;
}

static void check_triggers(void)
{
	unsigned char values[512];
	size_t index;
	unsigned int seed = 39;
	int presses;
	int value;

	/* the travel: rest, the end, and monotonic between */
	CHECK(trigger_travel(0) == 0 && trigger_travel(WII_TRIGGER_REST) == 0, "rest is no travel");
	CHECK(trigger_travel(WII_TRIGGER_FULL) == 0xFF && trigger_travel(255) == 0xFF, "the end is full travel");
	for (value = 0; value < 255; value++)
		CHECK(trigger_travel((unsigned char)(value + 1)) >= trigger_travel((unsigned char)value), "monotonic at %d", value);
	/* the press and its release: half the travel, a quarter, the click; only 0 or 255 */
	{
		BOOL pressed = FALSE;

		CHECK(trigger(&pressed, 104, FALSE) == 0 && !pressed, "short of half the travel");
		CHECK(trigger(&pressed, 106, FALSE) == 0xFF && pressed, "half the travel presses");
		CHECK(trigger(&pressed, 70, FALSE) == 0xFF && pressed, "above a quarter stays pressed");
		CHECK(trigger(&pressed, 67, FALSE) == 0 && !pressed, "a quarter lets go");
		CHECK(trigger(&pressed, 40, TRUE) == 0xFF && pressed, "the click presses wherever the value is");
		CHECK(trigger(&pressed, 0, FALSE) == 0 && !pressed, "rest lets go");
		for (value = 0; value < 256; value++)
		{
			BYTE out = trigger(&pressed, (unsigned char)value, FALSE);

			CHECK(out == 0 || out == 0xFF, "digital at %d", value);
		}
	}

	/* resting, or held anywhere short of half the travel, with noise: never a press */
	reset();
	plug(0, TRUE);
	game();
	for (index = 0; index < 300; index++)
	{
		seed = seed * 1103515245u + 12345u;
		values[index] = (unsigned char)((seed >> 16) % 105);
	}
	CHECK(fire_presses(values, 300, 33) == 0, "trigger noise short of the press point never fires");

	/* shaking about the press point (raw 98 to 112): one press */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	for (index = 0; index < 200; index++)
	{
		seed = seed * 1103515245u + 12345u;
		values[index] = (unsigned char)(98 + (seed >> 16) % 15);
	}
	presses = fire_presses(values, 200, 33);
	CHECK(presses == 1, "shaking at the press point fires once (%d)", presses);

	/* let go to above a quarter and pulled again: the same press; to below a
	quarter: a new one */
	{
		static const unsigned char partial[] = { 0, 150, 80, 150, 80, 150 };
		static const unsigned char repull[] = { 0, 150, 60, 150, 60, 150 };

		reset();
		plug(0, TRUE);
		game();
		frame(33);
		presses = fire_presses(partial, sizeof(partial), 33);
		CHECK(presses == 1, "partial let-go is one press (%d)", presses);
		reset();
		plug(0, TRUE);
		game();
		frame(33);
		presses = fire_presses(repull, sizeof(repull), 33);
		CHECK(presses == 3, "three pulls are three presses (%d)", presses);
	}

	/* a pull, held shaking by +-40, and let go: one press, one release */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	for (index = 0; index < 200; index++)
	{
		int raw = index < 10 ? (int)(index * 15) : 150;

		seed = seed * 1103515245u + 12345u;
		if (index >= 10)
			raw += (int)((seed >> 16) % 81) - 40;
		values[index] = (unsigned char)raw;
	}
	presses = fire_presses(values, 200, 33);
	CHECK(presses == 1, "a shaking pull fires once (%d)", presses);
	pads[0].trigger_r = 0;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_right_trigger] == 0, "let go releases");

	/* the click with the analog value dropping back (the pad's own reading
	after the click): still the one press */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	pads[0].trigger_r = 200;
	pads[0].buttons = WII_PAD_R;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_right_trigger] == 1, "click presses");
	pads[0].trigger_r = 120;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_right_trigger] >= 2, "click held with the analog value lower");

	/* the same pulls at 30, 60 and 144 frames a second: as many presses */
	{
		static const unsigned long frame_ms[] = { 33, 16, 7 };
		int counts[3];
		size_t rate;

		for (rate = 0; rate < 3; rate++)
		{
			unsigned long t;

			reset();
			plug(0, TRUE);
			game();
			frame(frame_ms[rate]);
			counts[rate] = 0;
			for (t = 0; t < 4000; t += frame_ms[rate])
			{
				/* four pulls a second: 150 ms in, 100 ms out */
				pads[0].trigger_r = (t % 250) < 150 ? 170 : 10;
				frame(frame_ms[rate]);
				if (buttons(0)[_gamepad_analog_button_right_trigger] == 1)
					counts[rate]++;
			}
		}
		CHECK(counts[0] == 16 && counts[1] == 16 && counts[2] == 16, "16 pulls fire 16 times at every rate (%d %d %d)",
			counts[0], counts[1], counts[2]);
	}

	/* the left trigger (grenade) goes the same way */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	pads[0].trigger_l = 25;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_left_trigger] == 0, "grenade trigger rest");
	pads[0].trigger_l = 160;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_left_trigger] == 1, "grenade trigger press");
	pads[0].trigger_l = 150;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_left_trigger] >= 2, "grenade trigger held");
}

/* ---------- held counts across frame rates */

static void check_hold_counts(void)
{
	static const unsigned long frame_ms[] = { 33, 16, 7 };
	byte last[3];
	int presses[3];
	size_t rate;

	for (rate = 0; rate < 3; rate++)
	{
		unsigned long t;

		reset();
		plug(0, TRUE);
		game();
		frame(frame_ms[rate]);
		pads[0].buttons = WII_PAD_X;
		presses[rate] = 0;
		for (t = 0; t < 1000; t += frame_ms[rate])
		{
			frame(frame_ms[rate]);
			if (buttons(0)[_gamepad_analog_button_x] == 1)
				presses[rate]++;
		}
		last[rate] = buttons(0)[_gamepad_analog_button_x];
	}
	CHECK(presses[0] == 1 && presses[1] == 1 && presses[2] == 1, "one press at every rate");
	/* a second's hold is 31 ticks at 30 Hz, give or take the last frame */
	for (rate = 0; rate < 3; rate++)
		CHECK(last[rate] >= 30 && last[rate] <= 32, "held ticks at %lu ms frames: %d", frame_ms[rate], last[rate]);
}

/* ---------- sticks */

static void check_sticks(void)
{
	int value;

	reset();
	plug(0, TRUE);
	game();
	frame(33);
	/* the dead zone: the pad's centre noise reads nothing */
	for (value = -27; value <= 27; value++)
	{
		pads[0].stick_x = pads[0].stick_y = pads[0].cstick_x = pads[0].cstick_y = (signed char)value;
		frame(33);
		CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_left].x == 0 &&
			input_globals.gamepad_states[0].sticks[_gamepad_stick_right].y == 0, "stick %d inside the dead zone", value);
	}
	/* the gate's edges are full, and up is up (forward, and look up before
	the engine's inversion) */
	pads[0].stick_x = 0;
	pads[0].stick_y = 100;
	pads[0].cstick_x = -100;
	pads[0].cstick_y = 100;
	frame(33);
	CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_left].y == 32767, "main stick up is full forward");
	CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_left].x == 0, "main stick x centred");
	CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_right].y == 32767, "C stick up is full up");
	CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_right].x <= -32766, "C stick left is full left");
	/* the gate's corners and beyond saturate rather than wrap */
	pads[0].stick_x = 127;
	pads[0].stick_y = -128;
	frame(33);
	CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_left].x == 32767 &&
		input_globals.gamepad_states[0].sticks[_gamepad_stick_left].y == -32768, "saturates");
	/* monotonic through the travel */
	{
		short previous = -32768;

		for (value = -128; value <= 127; value++)
		{
			pads[0].cstick_x = (signed char)value;
			frame(33);
			CHECK(input_globals.gamepad_states[0].sticks[_gamepad_stick_right].x >= previous, "monotonic at %d", value);
			previous = input_globals.gamepad_states[0].sticks[_gamepad_stick_right].x;
		}
	}
	/* the sticks are the same in both layers */
	menus();
	pads[0].stick_x = 50;
	frame(33);
	{
		short in_menus = input_globals.gamepad_states[0].sticks[_gamepad_stick_left].x;

		game();
		frame(33);
		CHECK(in_menus != 0 && input_globals.gamepad_states[0].sticks[_gamepad_stick_left].x == in_menus,
			"sticks do not change with the layer");
	}
}

/* ---------- switching layers */

static void check_layer_switches(void)
{
	/* crouching (Z) as the pause menu opens: no Back in the menu (which
	would close it), not even after a frame; Back once Z is pressed anew */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	pads[0].buttons = WII_PAD_Z;
	frame(33);
	CHECK(buttons(0)[_gamepad_binary_button_left_thumb] == 1, "Z crouches");
	menus();
	frame(33);
	CHECK(buttons(0)[_gamepad_binary_button_left_thumb] == 0, "crouch let go in the menus");
	CHECK(buttons(0)[_gamepad_binary_button_back] == 0, "Z held over the switch is not Back");
	frame(33);
	CHECK(buttons(0)[_gamepad_binary_button_back] == 0, "nor on the next frame");
	pads[0].buttons = 0;
	frame(33);
	pads[0].buttons = WII_PAD_Z;
	frame(33);
	CHECK(buttons(0)[_gamepad_binary_button_back] == 1, "Z pressed anew is Back");

	/* the D-pad held as the menu closes: no flashlight */
	reset();
	plug(0, TRUE);
	menus();
	frame(33);
	pads[0].buttons = WII_PAD_UP;
	frame(33);
	CHECK(buttons(0)[_gamepad_binary_button_dpad_up] == 1, "up in the menus");
	game();
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_white] == 0 && buttons(0)[_gamepad_binary_button_dpad_up] == 0,
		"up held over the switch is neither");
	pads[0].buttons = 0;
	frame(33);
	pads[0].buttons = WII_PAD_UP;
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_white] == 1, "up pressed anew is the flashlight");

	/* what keeps its meaning is not withheld: fire and A held over a switch */
	reset();
	plug(0, TRUE);
	game();
	frame(33);
	pads[0].buttons = WII_PAD_A;
	pads[0].trigger_r = 200;
	frame(33);
	menus();
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_a] >= 2 && buttons(0)[_gamepad_analog_button_right_trigger] >= 2,
		"A and fire stay held");
}

/* ---------- unplugging */

static void check_disconnects(void)
{
	HANDLE second;

	reset();
	plug(0, TRUE);
	plug(1, TRUE);
	game();
	frame(33);
	CHECK(input_globals.gamepad_handles[0] != NULL && input_globals.gamepad_handles[1] != NULL, "both ports open");
	second = input_globals.gamepad_handles[1];
	pads[0].buttons = WII_PAD_X | WII_PAD_Z;
	pads[0].trigger_r = 200;
	pads[1].buttons = WII_PAD_A;
	frame(33);
	frame(33);
	CHECK(buttons(0)[_gamepad_analog_button_right_trigger] >= 2 && buttons(0)[_gamepad_analog_button_x] >= 2 &&
		buttons(0)[_gamepad_binary_button_left_thumb] >= 2, "port 0 holds fire, action and crouch");

	/* port 0 unplugged: its controller is gone and holds nothing */
	plug(0, FALSE);
	frame(33);
	{
		const struct gamepad_state *state = &input_globals.gamepad_states[0];
		struct gamepad_state zero;

		memset(&zero, 0, sizeof(zero));

		CHECK(input_globals.gamepad_handles[0] == NULL, "port 0 closed");
		CHECK(!memcmp(state, &zero, sizeof(zero)), "port 0 holds nothing");
	}
	/* no other port takes its place */
	CHECK(input_globals.gamepad_handles[1] == second, "port 1 is still port 1");
	CHECK(buttons(1)[_gamepad_analog_button_a] >= 2, "port 1 still holds its A");
	CHECK(buttons(1)[_gamepad_analog_button_x] == 0 && buttons(1)[_gamepad_analog_button_right_trigger] == 0,
		"port 0's held buttons are not port 1's");
	CHECK(XInputOpen(XDEVICE_TYPE_GAMEPAD, 0, XDEVICE_NO_SLOT, NULL) == NULL &&
		last_error == ERROR_DEVICE_NOT_CONNECTED, "port 0 cannot be opened while unplugged");
	{
		XINPUT_STATE state;

		CHECK(XInputGetState((HANDLE)&controllers[0], &state) == ERROR_DEVICE_NOT_CONNECTED, "a stale handle reads nothing");
	}
	frame(33);
	frame(33);
	CHECK(input_globals.gamepad_handles[0] == NULL, "port 0 stays closed");

	/* back, with action still held: a fresh press, not the old hold (which
	would have been a weapon swap by now) */
	plug(0, TRUE);
	pads[0].buttons = WII_PAD_X;
	frame(33);
	CHECK(input_globals.gamepad_handles[0] != NULL, "port 0 back");
	CHECK(buttons(0)[_gamepad_analog_button_x] == 1, "action pressed afresh (%d)", buttons(0)[_gamepad_analog_button_x]);
	CHECK(buttons(0)[_gamepad_binary_button_left_thumb] == 0 && buttons(0)[_gamepad_analog_button_right_trigger] == 0,
		"what was let go of is not held");

	/* a port that was never plugged in never reads */
	CHECK(input_globals.gamepad_handles[2] == NULL && input_globals.gamepad_handles[3] == NULL, "ports 2 and 3 closed");
	CHECK(engine_errors == 0, "the engine reported no read failure");
}

/* ---------- determinism */

static unsigned long digest_run(void)
{
	unsigned long digest = 2166136261u;
	unsigned int seed = 7;
	int index;

	reset();
	plug(0, TRUE);
	frame(33);
	for (index = 0; index < 600; index++)
	{
		size_t byte_index;

		seed = seed * 1103515245u + 12345u;
		pads[0].buttons = (unsigned short)((seed >> 8) & 0x1F7F);
		pads[0].trigger_r = (unsigned char)(seed >> 3);
		pads[0].stick_x = (signed char)(seed >> 11);
		if (index % 50 == 0)
			halo_ui_pointer_update(index % 100 == 0, NULL);
		frame(16 + (seed >> 28));
		for (byte_index = 0; byte_index < sizeof(input_globals.gamepad_states[0]); byte_index++)
			digest = (digest ^ ((const byte *)&input_globals.gamepad_states[0])[byte_index]) * 16777619u;
	}
	return digest;
}

int main(void)
{
	unsigned long first;

	check_layers();
	check_actions();
	check_triggers();
	check_hold_counts();
	check_sticks();
	check_layer_switches();
	check_disconnects();
	first = digest_run();
	CHECK(digest_run() == first, "the same pad script reads the same");
	if (failures)
	{
		printf("FAILED %lu of %lu checks\n", failures, checks);
		return 1;
	}
	printf("ok %lu checks\n", checks);
	return 0;
}
