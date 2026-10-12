/*
XBOX_ADPCM.H

HWI-074: the game's Xbox ADPCM, decoded on the CPU as the mixer reads it,
one block at a time. The decoder HWI-022 chose (port/wii/content/hws_sound.c,
port/linux/src/dsound_sdl.c, tools/wii/media_formats.py decode_xbox_adpcm):
per channel and 36-byte block, a little-endian first sample and step index
(above 88 clamped), then 32 bytes of nibbles, low nibble first, alternating
between the channels four bytes at a time; the header's sample is the
block's first, the nibbles code the 63 after it, the 64th nibble only pads.
Plain C, no platform types: the Wii, the i686 host reference and the host
tests compile it alike.
*/

#ifndef __HALO_WII_XBOX_ADPCM_H
#define __HALO_WII_XBOX_ADPCM_H

#define XBOX_ADPCM_BLOCK_BYTES 36
#define XBOX_ADPCM_BLOCK_FRAMES 64

/* decodes one block of channels (1 or 2) channels, XBOX_ADPCM_BLOCK_BYTES
each, into XBOX_ADPCM_BLOCK_FRAMES interleaved frames */
void xbox_adpcm_decode_block(const unsigned char *block, unsigned int channels, short *frames);

/* decodes blocks whole blocks one after another (frames: blocks x 64 x channels) */
void xbox_adpcm_decode(const unsigned char *data, unsigned long blocks, unsigned int channels, short *frames);

#endif
