/*
WII_XINPUT.C

HWI-016D: the Xbox controller interface (XInput: XInitDevices,
XGetDeviceChanges, XInputOpen/Close/GetState/SetState and the debug
keyboard's queue) on the GameCube pad (wii_input.h). The engine's own input
(source/input/input_xbox.c) reads it every frame and hands the states to
its input abstraction, which the menus and the player read: nothing in the
engine changes but that an unplugged controller is let go of (HWI-039).

HWI-039: the pad has no black or white button and no stick clicks, which
the player's switch grenades, flashlight, crouch and zoom are on (the
profile's button layout, source/interface/player_ui.c). It has two layers:
while a menu is up it is the menus' controller, as HWI-016D mapped it; in
the game, Z and the D-pad press the buttons it lacks instead.

  GameCube pad      menus                  the game
  A, B, X, Y        A, B, X, Y (analog buttons, fully pressed)
  Start             Start
  Z                 Back                   left thumbstick click (crouch)
  control pad up    D-pad up               white (flashlight)
  control pad down  D-pad down             Back (scoreboard)
  control pad left  D-pad left             black (switch grenades)
  control pad right D-pad right            right thumbstick click (zoom)
  main stick        left thumbstick
  C stick           right thumbstick
  L, R (analog)     left, right triggers (grenade, fire)

The game's are buttons of the Xbox controller, not actions, so the
profile's layout (and its presets) apply to them as to the controller's;
in the game the D-pad moves nothing (its movement is the main stick's).
The menus say each frame whether they are up (halo_ui_pointer_update,
source/interface/ui_widget.c); the layer is that frame's for the next read.
A Z or control pad press held as the layers switch is withheld until let go
of, so it never means the other layer's button.

The engine turns the states into presses (source/input/input_xbox.c): a
button's count is 1 on the read it goes down and from then on the 30 Hz
ticks it has been held, by the clock, so a press is seen once and held
times do not depend on the frame rate. Each read takes the pad's state from
that frame's scan (XGetDeviceChanges, once a frame), never another's:
nothing is accumulated between reads. The sticks' dead zone (9000 of 32767)
and the look inversion are the engine's.

The triggers are pressed here, not by the engine's analog threshold (which
follows the value 32 below its highest: a GameCube trigger held shaking
crosses it, and fires again). Their travel is libogc's PAD_Clamp region
(30 to 180, below it rest) taken as the Xbox's 0 to 255; a trigger is
pressed from half its travel or its digital click, and let go of at a
quarter, and reads fully pressed (255) or not at all (0), so the engine
sees one press for each pull.

Ports report connection changes as XGetDeviceChanges asks; with no pad
source registered (the i686 host reference) no gamepad is ever connected.
A pad is always its own port's controller: one unplugged reads as nothing
until it is back, and no other port takes its place. The debug keyboard is
never connected. Rumble is accepted and not played.

Limits: one layer for every port, picked by the first player's menus
(ui_mouse_menu: widgets of no player or of player 0), so in split screen a
second player's own menu leaves the pads in the game's layer and the first
player's puts them all in the menus'; the switch lags a frame (withheld
inputs keep it from meaning anything); in the game the D-pad neither moves
nor (southpaw presets) looks; the sticks are taken to reach +-100, which the
C stick of a real pad may fall short of (a full turn rate wants measuring
on hardware).

Compiled for the host with WII_XINPUT_STANDALONE (tools/wii/
gamecube_input_check.c) against the XDK headers alone.
*/

#ifndef WII_XINPUT_STANDALONE
#include "platform.h"
#include "wii_platform.h"
#endif
#include "wii_input.h"
#include "halo_ui_pointer.h"

#include <string.h>

/* ---------- constants */

enum
{
	/* libogc's PAD_Clamp: a trigger's analog travel */
	WII_TRIGGER_REST = 30,
	WII_TRIGGER_FULL = 180,
	/* where in its travel (0 to 255) a trigger is pressed and let go of */
	WII_TRIGGER_PRESS = 0x80,
	WII_TRIGGER_RELEASE = 0x40,

	/* the pad's inputs the layers give different meanings */
	WII_PAD_LAYERED = WII_PAD_Z | WII_PAD_UP | WII_PAD_DOWN | WII_PAD_LEFT | WII_PAD_RIGHT,

	NO_ANALOG_BUTTON = -1
};

/* ---------- globals */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct wii_controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
	/* the layer of the last read, and the layered inputs held since it
	switched */
	BOOL menus;
	unsigned short withheld;
	/* the triggers' presses (left, right) */
	BOOL triggers[2];
};

struct pad_binding
{
	unsigned short pad;
	WORD xbox_button;
	short xbox_analog_button;
};

static wii_pad_scan_proc pad_scan;
static wii_pad_read_proc pad_read;
static struct wii_controller controllers[WII_PAD_PORTS];
static DWORD reported_gamepads;
static unsigned int scanned_mask;
static struct wii_xinput_report xinput_report;
static BOOL menus_active = TRUE;

static const struct pad_binding menu_bindings[] =
{
	{ WII_PAD_A, 0, XINPUT_GAMEPAD_A },
	{ WII_PAD_B, 0, XINPUT_GAMEPAD_B },
	{ WII_PAD_X, 0, XINPUT_GAMEPAD_X },
	{ WII_PAD_Y, 0, XINPUT_GAMEPAD_Y },
	{ WII_PAD_START, XINPUT_GAMEPAD_START, NO_ANALOG_BUTTON },
	{ WII_PAD_Z, XINPUT_GAMEPAD_BACK, NO_ANALOG_BUTTON },
	{ WII_PAD_UP, XINPUT_GAMEPAD_DPAD_UP, NO_ANALOG_BUTTON },
	{ WII_PAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN, NO_ANALOG_BUTTON },
	{ WII_PAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT, NO_ANALOG_BUTTON },
	{ WII_PAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT, NO_ANALOG_BUTTON },
};

static const struct pad_binding game_bindings[] =
{
	{ WII_PAD_A, 0, XINPUT_GAMEPAD_A },
	{ WII_PAD_B, 0, XINPUT_GAMEPAD_B },
	{ WII_PAD_X, 0, XINPUT_GAMEPAD_X },
	{ WII_PAD_Y, 0, XINPUT_GAMEPAD_Y },
	{ WII_PAD_START, XINPUT_GAMEPAD_START, NO_ANALOG_BUTTON },
	{ WII_PAD_Z, XINPUT_GAMEPAD_LEFT_THUMB, NO_ANALOG_BUTTON },
	{ WII_PAD_UP, 0, XINPUT_GAMEPAD_WHITE },
	{ WII_PAD_DOWN, XINPUT_GAMEPAD_BACK, NO_ANALOG_BUTTON },
	{ WII_PAD_LEFT, 0, XINPUT_GAMEPAD_BLACK },
	{ WII_PAD_RIGHT, XINPUT_GAMEPAD_RIGHT_THUMB, NO_ANALOG_BUTTON },
};

/* ---------- source */

void wii_xinput_set_pad_source(
	wii_pad_scan_proc scan,
	wii_pad_read_proc read)
{
	pad_scan = scan;
	pad_read = read;
}

void wii_xinput_report_get(
	struct wii_xinput_report *report)
{
	*report = xinput_report;
}

/* the menus' pointer (halo_ui_pointer.h): the Wii has none. The menus call
this once a frame with whether they are up, which picks the pad's layer. */
int halo_ui_pointer_update(
	int active,
	struct halo_ui_pointer *pointer)
{
	(void)pointer;
	menus_active = active != 0;
	return 0;
}

/* ---------- mapping */

static SHORT thumb(
	int value)
{
	/* the pad's sticks reach about +-100 (+-110 at the gate's corners) */
	long scaled = (long)value * 32767 / 100;

	if (scaled > 32767)
		scaled = 32767;
	if (scaled < -32768)
		scaled = -32768;
	return (SHORT)scaled;
}

static int trigger_travel(
	unsigned char value)
{
	if (value >= WII_TRIGGER_FULL)
		return 0xFF;
	if (value <= WII_TRIGGER_REST)
		return 0;
	return (value - WII_TRIGGER_REST) * 0xFF / (WII_TRIGGER_FULL - WII_TRIGGER_REST);
}

static BYTE trigger(
	BOOL *pressed,
	unsigned char value,
	BOOL clicked)
{
	int travel = trigger_travel(value);

	if (clicked || travel >= WII_TRIGGER_PRESS)
		*pressed = TRUE;
	else if (travel <= WII_TRIGGER_RELEASE)
		*pressed = FALSE;
	return *pressed ? 0xFF : 0;
}

static void pad_to_gamepad(
	const struct wii_pad_state *pad,
	struct wii_controller *controller,
	XINPUT_GAMEPAD *gamepad)
{
	const struct pad_binding *bindings = menus_active ? menu_bindings : game_bindings;
	size_t binding_count = menus_active ?
		sizeof(menu_bindings) / sizeof(menu_bindings[0]) :
		sizeof(game_bindings) / sizeof(game_bindings[0]);
	unsigned short buttons;
	size_t index;

	if (controller->menus != menus_active)
	{
		controller->withheld |= pad->buttons & WII_PAD_LAYERED;
		controller->menus = menus_active;
	}
	controller->withheld &= pad->buttons;
	buttons = pad->buttons & ~controller->withheld;

	memset(gamepad, 0, sizeof(*gamepad));
	for (index = 0; index < binding_count; index++)
	{
		if (!(buttons & bindings[index].pad))
			continue;
		gamepad->wButtons |= bindings[index].xbox_button;
		if (bindings[index].xbox_analog_button != NO_ANALOG_BUTTON)
			gamepad->bAnalogButtons[bindings[index].xbox_analog_button] = 0xFF;
	}
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] =
		trigger(&controller->triggers[0], pad->trigger_l, (pad->buttons & WII_PAD_L) != 0);
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] =
		trigger(&controller->triggers[1], pad->trigger_r, (pad->buttons & WII_PAD_R) != 0);
	gamepad->sThumbLX = thumb(pad->stick_x);
	gamepad->sThumbLY = thumb(pad->stick_y);
	gamepad->sThumbRX = thumb(pad->cstick_x);
	gamepad->sThumbRY = thumb(pad->cstick_y);
}

/* ---------- XAPI */

VOID WINAPI XInitDevices(
	DWORD preallocation_type_count,
	PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
	memset(controllers, 0, sizeof(controllers));
	reported_gamepads = 0;
	scanned_mask = 0;
	menus_active = TRUE;
}

BOOL WINAPI XGetDeviceChanges(
	PXPP_DEVICE_TYPE device_type,
	PDWORD insertions,
	PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		DWORD connected = 0;

		/* (once a frame: input_xbox.c asks for the gamepads' changes first,
		then reads each open one) */
		if (pad_scan)
		{
			scanned_mask = pad_scan();
			xinput_report.scans++;
			connected = scanned_mask & ((1u << WII_PAD_PORTS) - 1);
		}
		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(
	PXPP_DEVICE_TYPE device_type,
	DWORD port,
	DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < WII_PAD_PORTS && (reported_gamepads & (1u << port)))
	{
		/* (a pad put back starts afresh: nothing held, nothing withheld) */
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
		controllers[port].menus = menus_active;
		return (HANDLE)&controllers[port];
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(
	HANDLE device)
{
	struct wii_controller *controller = (struct wii_controller *)device;

	if (controller)
		controller->open = FALSE;
}

static int controller_port(
	HANDLE device)
{
	int port;

	for (port = 0; port < WII_PAD_PORTS; port++)
	{
		if (device == (HANDLE)&controllers[port] && controllers[port].open)
			return port;
	}
	return -1;
}

DWORD WINAPI XInputGetState(
	HANDLE device,
	PXINPUT_STATE state)
{
	int port = controller_port(device);
	struct wii_pad_state pad;

	memset(state, 0, sizeof(*state));
	if (port < 0 || !pad_read || !(scanned_mask & (1u << port)))
		return ERROR_DEVICE_NOT_CONNECTED;
	memset(&pad, 0, sizeof(pad));
	pad_read(port, &pad);
	pad_to_gamepad(&pad, &controllers[port], &state->Gamepad);
	if (memcmp(&state->Gamepad, &controllers[port].previous, sizeof(state->Gamepad)))
	{
		controllers[port].packet_number++;
		controllers[port].previous = state->Gamepad;
		if (port == 0)
			xinput_report.packets++;
	}
	state->dwPacketNumber = controllers[port].packet_number;
	if (port == 0)
	{
		xinput_report.reads++;
		xinput_report.last_xbox_buttons = state->Gamepad.wButtons;
		memcpy(xinput_report.last_analog, state->Gamepad.bAnalogButtons, sizeof(xinput_report.last_analog));
	}
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(
	HANDLE device,
	PXINPUT_FEEDBACK feedback)
{
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	/* rumble is accepted, not played (HWI-017) */
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return controller_port(device) < 0 ? ERROR_DEVICE_NOT_CONNECTED : ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(
	PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(
	PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	/* no debug keyboard */
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}
