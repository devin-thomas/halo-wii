/*
WII_XINPUT.C

HWI-016D: the Xbox controller interface (XInput: XInitDevices,
XGetDeviceChanges, XInputOpen/Close/GetState/SetState and the debug
keyboard's queue) on the GameCube pad (wii_input.h). The engine's own input
(source/input/input_xbox.c) reads it every frame and hands the states to
its input abstraction, which the menus and the player read: nothing in the
engine changes.

The minimum the menus need, recorded as such (the full mapping, its
sensitivity, inversion, dead zones, rumble and reconnects are HWI-017 and
HWI-028):

  GameCube pad      Xbox controller
  A, B, X, Y        A, B, X, Y (analog buttons, fully pressed)
  Start             Start
  Z                 Back
  control pad       D-pad
  main stick        left thumbstick
  C stick           right thumbstick
  L, R (analog)     left, right triggers
  (none)            black, white, thumbstick clicks

Ports report connection changes as XGetDeviceChanges asks; with no pad
source registered (the i686 host reference) no gamepad is ever connected.
The debug keyboard is never connected. Rumble is accepted and not played.
*/

#include "platform.h"
#include "wii_platform.h"
#include "wii_input.h"

#include <string.h>

/* ---------- globals */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct wii_controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static wii_pad_scan_proc pad_scan;
static wii_pad_read_proc pad_read;
static struct wii_controller controllers[WII_PAD_PORTS];
static DWORD reported_gamepads;
static unsigned int scanned_mask;
static struct wii_xinput_report xinput_report;

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

static void pad_to_gamepad(
	const struct wii_pad_state *pad,
	XINPUT_GAMEPAD *gamepad)
{
	static const struct
	{
		unsigned short pad;
		WORD xbox;
	} digital[] =
	{
		{ WII_PAD_UP, XINPUT_GAMEPAD_DPAD_UP },
		{ WII_PAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
		{ WII_PAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
		{ WII_PAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
		{ WII_PAD_START, XINPUT_GAMEPAD_START },
		{ WII_PAD_Z, XINPUT_GAMEPAD_BACK },
	};
	static const struct
	{
		unsigned short pad;
		int xbox;
	} analog[] =
	{
		{ WII_PAD_A, XINPUT_GAMEPAD_A },
		{ WII_PAD_B, XINPUT_GAMEPAD_B },
		{ WII_PAD_X, XINPUT_GAMEPAD_X },
		{ WII_PAD_Y, XINPUT_GAMEPAD_Y },
	};
	size_t index;

	memset(gamepad, 0, sizeof(*gamepad));
	for (index = 0; index < sizeof(digital) / sizeof(digital[0]); index++)
	{
		if (pad->buttons & digital[index].pad)
			gamepad->wButtons |= digital[index].xbox;
	}
	for (index = 0; index < sizeof(analog) / sizeof(analog[0]); index++)
	{
		if (pad->buttons & analog[index].pad)
			gamepad->bAnalogButtons[analog[index].xbox] = 0xFF;
	}
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = pad->trigger_l;
	gamepad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = pad->trigger_r;
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
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
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
	pad_to_gamepad(&pad, &state->Gamepad);
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
