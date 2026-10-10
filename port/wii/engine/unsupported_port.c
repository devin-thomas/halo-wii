/*
UNSUPPORTED_PORT.C

Entry points of the native ports' platform layer (port/linux/src) that the
engine links against and the Wii engine build does not implement yet
(HWI-015): the renderer's and the desktop's hooks, Bink video, sound
streams, input devices, the high-resolution HUD and text, the PC menus and
internet play. The Xbox SDK's own entry points are in unsupported_sdk.c.

Each reports itself as unsupported (wii_unsupported) and fails the call: it
returns NULL, 0, FALSE or an empty result, which the engine handles as "not
there" (no movie, no device, no asset, no peer). Three return a value that is
not a failure, because the engine divides or lays out by them; each says
why. Nothing here claims a working subsystem.

The prototypes are the ones the engine units declare (copied where their
header cannot be compiled without SDL).
*/

#include "platform.h"
#include "wii_platform.h"
#include "p2p.h"
#include "text_hires.h"
#include "halo_menus.h"
#include "halo_ui_pointer.h"
#include "halo_keyboard.h"

#include <string.h>

/* ---------- Direct3D state the XDK's inline functions keep (xdk_d3d8.h)

Storage, not a device: the inline setters write here before calling the
unsupported device entry points. */

DWORD D3D__RenderState[D3DRS_MAX];
DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
WORD *D3D__IndexData;

typedef long hresult;

hresult __stdcall D3DXGetErrorStringA(hresult error_result, char *buffer, unsigned long buffer_length)
{
	wii_unsupported("d3d8", "D3DXGetErrorStringA");
	if (buffer && buffer_length)
		buffer[0] = 0;
	return E_FAIL;
}

volatile unsigned int *d3d_find_flipcount(void)
{
	wii_unsupported("d3d8", "d3d_find_flipcount");
	return NULL;
}

/* ---------- the renderer's hooks (halo_linux_source_fixups.h) */

/* the engine lays its 640x480 interface out by this width: 640, the Xbox's
own, which is a layout constant, not a display */
long halo_screen_width(void)
{
	wii_unsupported("render", "halo_screen_width");
	return 640;
}

long halo_screen_commit(void)
{
	wii_unsupported("render", "halo_screen_commit");
	return 640;
}

/* shadow maps are drawn this many pixels per texel: 1, the Xbox's */
long halo_shadow_map_scale(void)
{
	wii_unsupported("render", "halo_shadow_map_scale");
	return 1;
}

void halo_screen_ui_offset(unsigned char centered)
{
	wii_unsupported("render", "halo_screen_ui_offset");
}

void halo_screen_anti_alias(short x0, short y0, short x1, short y1)
{
	wii_unsupported("render", "halo_screen_anti_alias");
}

void halo_vertex_shader_lighting(unsigned long handle)
{
	wii_unsupported("render", "halo_vertex_shader_lighting");
}

int halo_interpolation_enabled(void)
{
	wii_unsupported("render", "halo_interpolation_enabled");
	return 0;
}

void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order)
{
	wii_unsupported("render", "halo_custom_edition_texels_channels");
}

void halo_custom_edition_texels_forget(void)
{
	wii_unsupported("render", "halo_custom_edition_texels_forget");
}

/* ---------- Bink video (no decoder; the RAD SDK is proprietary)

BinkOpen fails, and the engine skips the movie as it does a missing file. */

typedef void *(__stdcall *rad_memory_allocate_proc)(unsigned long size);
typedef void (__stdcall *rad_memory_free_proc)(void *memory);
typedef void *(__stdcall *bink_sound_system_open_proc)(unsigned long param);
typedef struct BINK *HBINK;

void __stdcall RADSetMemory(rad_memory_allocate_proc allocate, rad_memory_free_proc release)
{
	wii_unsupported("bink", "RADSetMemory");
}

void *__stdcall BinkOpenDirectSound(unsigned long param)
{
	wii_unsupported("bink", "BinkOpenDirectSound");
	return NULL;
}

long __stdcall BinkSetSoundSystem(bink_sound_system_open_proc open, unsigned long param)
{
	wii_unsupported("bink", "BinkSetSoundSystem");
	return 0;
}

void __stdcall BinkSetIOSize(unsigned long io_size)
{
	wii_unsupported("bink", "BinkSetIOSize");
}

HBINK __stdcall BinkOpen(const char *name, unsigned long flags)
{
	wii_unsupported("bink", "BinkOpen");
	return NULL;
}

void __stdcall BinkClose(HBINK bink)
{
	wii_unsupported("bink", "BinkClose");
}

long __stdcall BinkDoFrame(HBINK bink)
{
	wii_unsupported("bink", "BinkDoFrame");
	return 0;
}

void __stdcall BinkNextFrame(HBINK bink)
{
	wii_unsupported("bink", "BinkNextFrame");
}

long __stdcall BinkWait(HBINK bink)
{
	wii_unsupported("bink", "BinkWait");
	return 0;
}

long __stdcall BinkCopyToBuffer(HBINK bink, void *destination, long destination_pitch,
	unsigned long destination_height, unsigned long destination_x, unsigned long destination_y,
	unsigned long flags)
{
	wii_unsupported("bink", "BinkCopyToBuffer");
	return 0;
}

void __stdcall BinkGetSummary(HBINK bink, void *summary)
{
	wii_unsupported("bink", "BinkGetSummary");
}

void __stdcall BinkGetRealtime(HBINK bink, void *realtime, unsigned long frame_count)
{
	wii_unsupported("bink", "BinkGetRealtime");
}

/* ---------- sound streams (source/sound/sound_dsound_xbox.c) */

void __stdcall DirectSoundStopStream(LPDIRECTSOUNDSTREAM stream)
{
	wii_unsupported("dsound", "DirectSoundStopStream");
}

/* 0: the stream's voice has finished, which ends the channel */
unsigned long __stdcall DirectSoundGetStreamVoiceStatus(LPDIRECTSOUNDSTREAM stream)
{
	wii_unsupported("dsound", "DirectSoundGetStreamVoiceStatus");
	return 0;
}

/* ---------- input devices (no device is ever reported) */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

void halo_input_name(int input, char *name, size_t size)
{
	wii_unsupported("input", "halo_input_name");
	if (name && size)
		name[0] = 0;
}

unsigned long halo_keyboard_actions(short controller_index)
{
	wii_unsupported("input", "halo_keyboard_actions");
	return 0;
}

int halo_linux_mouse_aiming(short gamepad_index)
{
	wii_unsupported("input", "halo_linux_mouse_aiming");
	return 0;
}

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	wii_unsupported("input", "halo_linux_mouse_look");
	return 0;
}

int halo_ui_pointer_update(int menus_active, struct halo_ui_pointer *pointer)
{
	wii_unsupported("input", "halo_ui_pointer_update");
	return 0;
}

void test_input_hold_action(int hold)
{
	wii_unsupported("input", "test_input_hold_action");
}

/* ---------- high-resolution HUD and text (port/assets; not staged) */

long hud_hires_asset_count(void)
{
	wii_unsupported("hires_assets", "hud_hires_asset_count");
	return 0;
}

long hud_hires_asset_bitmap(long asset)
{
	wii_unsupported("hires_assets", "hud_hires_asset_bitmap");
	return -1;
}

long hud_hires_asset_fits(long asset, long width, long height)
{
	wii_unsupported("hires_assets", "hud_hires_asset_fits");
	return 0;
}

char const *hud_hires_asset_tag(long asset)
{
	wii_unsupported("hires_assets", "hud_hires_asset_tag");
	return "";
}

long text_hires_font(char const *tag_name, float cap_height, float oversample)
{
	wii_unsupported("hires_assets", "text_hires_font");
	return -1;
}

int text_hires_covers(long font, unsigned long code)
{
	wii_unsupported("hires_assets", "text_hires_covers");
	return 0;
}

int text_hires_glyph(long font, unsigned long code, struct text_hires_glyph *glyph)
{
	wii_unsupported("hires_assets", "text_hires_glyph");
	return 0;
}

void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height)
{
	wii_unsupported("hires_assets", "text_hires_register_atlas");
}

/* ---------- the PC menus (port/linux/src/menu_files.c) */

struct halo_menus const *halo_menus_load(void)
{
	wii_unsupported("menus", "halo_menus_load");
	return NULL;
}

long halo_menus_utf16(char const *utf8, unsigned short *out, long capacity)
{
	wii_unsupported("menus", "halo_menus_utf16");
	return 0;
}

void halo_menus_log(char const *file, long line, char const *message, char const *detail)
{
	wii_unsupported("menus", "halo_menus_log");
}

void halo_menus_art_register(void const *texture, char const *png)
{
	wii_unsupported("menus", "halo_menus_art_register");
}

void halo_menus_art_forget(void)
{
	wii_unsupported("menus", "halo_menus_art_forget");
}

/* ---------- the desktop (port/linux/src/sdl_platform.c) */

void platform_binding_capture_begin(void)
{
	wii_unsupported("desktop", "platform_binding_capture_begin");
}

int platform_binding_capture_poll(int *input)
{
	wii_unsupported("desktop", "platform_binding_capture_poll");
	return 0;
}

int platform_clipboard_get(char *text, int size)
{
	wii_unsupported("desktop", "platform_clipboard_get");
	return 0;
}

void platform_clipboard_set(char const *text)
{
	wii_unsupported("desktop", "platform_clipboard_set");
}

void platform_display_apply(void)
{
	wii_unsupported("desktop", "platform_display_apply");
}

int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	wii_unsupported("desktop", "platform_display_resolutions");
	return 0;
}

int platform_window_sizes(long *widths, long *heights, int maximum)
{
	wii_unsupported("desktop", "platform_window_sizes");
	return 0;
}

BOOL platform_offer_game_data(const char *destination)
{
	wii_unsupported("desktop", "platform_offer_game_data");
	return FALSE;
}

void platform_request_quit(void)
{
	wii_unsupported("desktop", "platform_request_quit");
}

void platform_scoreboard_scroll(int open, long *notches, long *pages)
{
	wii_unsupported("desktop", "platform_scoreboard_scroll");
	if (notches)
		*notches = 0;
	if (pages)
		*pages = 0;
}

void platform_show_message(char const *title, char const *message)
{
	wii_unsupported("desktop", "platform_show_message");
	wii_log("platform: message \"%s\": %s\n", title ? title : "", message ? message : "");
}

void platform_text_field(int typing)
{
	wii_unsupported("desktop", "platform_text_field");
}

void platform_text_typing(int typing)
{
	wii_unsupported("desktop", "platform_text_typing");
}

/* ---------- internet play (port/linux/src/p2p*.c) */

int p2p_join_invite(const char *text)
{
	wii_unsupported("p2p", "p2p_join_invite");
	return 0;
}

int p2p_peer_address(const unsigned char *identifier, unsigned long *address)
{
	wii_unsupported("p2p", "p2p_peer_address");
	return 0;
}

unsigned long p2p_peer_endpoint_address(unsigned long virtual_address)
{
	wii_unsupported("p2p", "p2p_peer_endpoint_address");
	return 0;
}

void p2p_set_hosting_allowed(int allowed)
{
	wii_unsupported("p2p", "p2p_set_hosting_allowed");
}

int p2p_invite_link(char *link, int size)
{
	wii_unsupported("p2p", "p2p_invite_link");
	if (link && size > 0)
		link[0] = 0;
	return 0;
}

void p2p_set_game_player_counts(int count, int maximum)
{
	wii_unsupported("p2p", "p2p_set_game_player_counts");
}

void p2p_set_hosting_public(int public)
{
	wii_unsupported("p2p", "p2p_set_hosting_public");
}

void p2p_set_hosting_password(const char *password)
{
	wii_unsupported("p2p", "p2p_set_hosting_password");
}

void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int open,
	int in_progress, int has_teams)
{
	wii_unsupported("p2p", "p2p_set_game_listing");
}

void p2p_lobby_browse(int on)
{
	wii_unsupported("p2p", "p2p_lobby_browse");
}

void p2p_lobby_refresh(void)
{
	wii_unsupported("p2p", "p2p_lobby_refresh");
}

int p2p_lobby_games(struct p2p_listing *games, int maximum_count)
{
	wii_unsupported("p2p", "p2p_lobby_games");
	return 0;
}

void p2p_lobby_mark_failed(const unsigned char *identifier)
{
	wii_unsupported("p2p", "p2p_lobby_mark_failed");
}

int p2p_listing_unlock(struct p2p_listing *listing, const char *password)
{
	wii_unsupported("p2p", "p2p_listing_unlock");
	return 0;
}

void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	wii_unsupported("p2p", "p2p_discord_sanitize");
	if (destination && size > 0)
		destination[0] = 0;
}

void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{
	wii_unsupported("p2p", "p2p_discord_identity");
	if (id && id_size > 0)
		id[0] = 0;
	if (name && name_size > 0)
		name[0] = 0;
}

void p2p_hardware_id(char *hex, int size)
{
	wii_unsupported("p2p", "p2p_hardware_id");
	if (hex && size > 0)
		hex[0] = 0;
}

void p2p_hardware_id_sanitize(char *destination, int size, const char *source)
{
	wii_unsupported("p2p", "p2p_hardware_id_sanitize");
	if (destination && size > 0)
		destination[0] = 0;
}
