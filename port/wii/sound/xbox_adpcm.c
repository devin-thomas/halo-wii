/*
XBOX_ADPCM.C

HWI-074: Xbox ADPCM blocks to 16-bit PCM (xbox_adpcm.h). The same arithmetic
as port/wii/content/hws_sound.c hws_decode_adpcm (HWI-022's bit-exact
decoder), per block, so the mixer decodes only the block it reads.
*/

#include "xbox_adpcm.h"

static const signed char index_table[16] =
{
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8,
};

static const short step_table[89] =
{
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
	88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598,
	658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
	3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
	16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

void xbox_adpcm_decode_block(const unsigned char *block, unsigned int channels, short *frames)
{
	unsigned int channel;

	for (channel = 0; channel < channels; channel++)
	{
		/* the block header word, explicitly little-endian */
		const unsigned char *header = block + 4 * channel;
		long predictor = (long)(header[0] | ((unsigned int)header[1] << 8));
		int index = header[2] > 88 ? 88 : header[2];
		unsigned int group;

		if (predictor & 0x8000)
			predictor -= 0x10000;
		frames[channel] = (short)predictor;
		for (group = 0; group < 8; group++)
		{
			const unsigned char *nibbles = block + 4 * channels + (group * channels + channel) * 4;
			unsigned int byte;

			for (byte = 0; byte < 4; byte++)
			{
				unsigned int value = nibbles[byte];
				unsigned int position = group * 8 + byte * 2 + 1;
				unsigned int half;

				for (half = 0; half < 2; half++, position++)
				{
					unsigned int nibble = half ? value >> 4 : value & 15u;
					long step, difference;

					/* the block's last nibble pads it */
					if (position >= XBOX_ADPCM_BLOCK_FRAMES)
						continue;
					step = step_table[index];
					difference = step >> 3;
					if (nibble & 1)
						difference += step >> 2;
					if (nibble & 2)
						difference += step >> 1;
					if (nibble & 4)
						difference += step;
					predictor += (nibble & 8) ? -difference : difference;
					if (predictor > 32767)
						predictor = 32767;
					if (predictor < -32768)
						predictor = -32768;
					index += index_table[nibble];
					if (index < 0)
						index = 0;
					if (index > 88)
						index = 88;
					frames[position * channels + channel] = (short)predictor;
				}
			}
		}
	}
}

void xbox_adpcm_decode(const unsigned char *data, unsigned long blocks, unsigned int channels, short *frames)
{
	unsigned long block;

	for (block = 0; block < blocks; block++)
	{
		xbox_adpcm_decode_block(data + block * XBOX_ADPCM_BLOCK_BYTES * channels, channels,
			frames + block * XBOX_ADPCM_BLOCK_FRAMES * channels);
	}
}
