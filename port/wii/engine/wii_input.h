/*
WII_INPUT.H

HWI-016D: the Xbox controller interface (XInput) the engine's input reads
(source/input/input_xbox.c, then input_abstraction.c), implemented on the
GameCube pad (wii_xinput.c). The pad itself is read by whoever registers a
source: the Wii driver (libogc's PAD) in engine.dol; the i686 host reference
registers none, so no gamepad is ever connected there, as before.

C types only, so the driver can include it beside libogc's headers.
*/

#ifndef __HALO_WII_INPUT_H
#define __HALO_WII_INPUT_H

/* libogc's PAD_BUTTON_* bits (ogc/pad.h) */
enum
{
	WII_PAD_LEFT = 0x0001,
	WII_PAD_RIGHT = 0x0002,
	WII_PAD_DOWN = 0x0004,
	WII_PAD_UP = 0x0008,
	WII_PAD_Z = 0x0010,
	WII_PAD_R = 0x0020,
	WII_PAD_L = 0x0040,
	WII_PAD_A = 0x0100,
	WII_PAD_B = 0x0200,
	WII_PAD_X = 0x0400,
	WII_PAD_Y = 0x0800,
	WII_PAD_START = 0x1000,
};

enum
{
	WII_PAD_PORTS = 4,
};

struct wii_pad_state
{
	int connected;
	unsigned short buttons;
	signed char stick_x, stick_y;   /* main stick, about -100..100 */
	signed char cstick_x, cstick_y; /* C stick */
	unsigned char trigger_l, trigger_r;
};

/* reads every port once (called once per input update, before the ports are
read); returns the connected ports' mask */
typedef unsigned int (*wii_pad_scan_proc)(void);
/* one port's state as the last scan read it */
typedef void (*wii_pad_read_proc)(int port, struct wii_pad_state *state);

void wii_xinput_set_pad_source(wii_pad_scan_proc scan, wii_pad_read_proc read);

/* what the engine was last handed for port 0 (the boot mode's log) */
struct wii_xinput_report
{
	unsigned long scans;
	unsigned long reads;
	unsigned long packets;          /* port 0 state changes */
	unsigned short last_xbox_buttons; /* XINPUT_GAMEPAD wButtons */
	unsigned char last_analog[8];   /* bAnalogButtons */
};

void wii_xinput_report_get(struct wii_xinput_report *report);

#endif
