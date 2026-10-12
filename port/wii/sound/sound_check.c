/*
SOUND_CHECK.C

HWI-074: a host check of the Wii's sound pack reader and ADPCM decoder
(tools/wii/test_sound.py): the same sound_pack.c and xbox_adpcm.c the Wii
compiles, with the log and clock the platform layer would give them.

  sound_check decode <channels> <file>    decode a raw Xbox ADPCM payload:
                                          DECODE frames=<n> crc=<crc of LE PCM>
  sound_check pack <pack> <map name>      open it, read every entry (verified,
                                          SOUND_DECODE lines), then the refusals:
                                          READ offset=<hex> size=<n> result=<1|0|-1>
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wii_platform.h"
#include "wii_sound.h"
#include "xbox_adpcm.h"

void wii_log(const char *format, ...)
{
	va_list list;

	va_start(list, format);
	vprintf(format, list);
	va_end(list);
}

unsigned long long wii_time_microseconds(void)
{
	return 0;
}

static unsigned long crc_table[256];

static unsigned long crc_add(unsigned long crc, const unsigned char *bytes, unsigned long count)
{
	if (!crc_table[1])
	{
		for (unsigned long index = 0; index < 256; index++)
		{
			unsigned long value = index;

			for (int bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xEDB88320UL ^ (value >> 1) : value >> 1;
			crc_table[index] = value;
		}
	}
	crc = ~crc & 0xFFFFFFFFUL;
	while (count--)
		crc = crc_table[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

static unsigned char *read_file(const char *path, unsigned long *size)
{
	FILE *file = fopen(path, "rb");
	unsigned char *data;
	long length;

	if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET))
		return NULL;
	data = malloc((size_t)length + 1);
	if (data && fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		free(data);
		data = NULL;
	}
	fclose(file);
	*size = (unsigned long)length;
	return data;
}

static unsigned long be32(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) | ((unsigned long)bytes[2] << 8) |
		bytes[3];
}

static int decode(unsigned int channels, const char *path)
{
	unsigned long size, blocks, crc = 0;
	unsigned char *data = read_file(path, &size);

	if (!data || (channels != 1 && channels != 2))
		return 2;
	blocks = size / (XBOX_ADPCM_BLOCK_BYTES * channels);
	for (unsigned long block = 0; block < blocks; block++)
	{
		short frames[XBOX_ADPCM_BLOCK_FRAMES * 2];
		unsigned char little[XBOX_ADPCM_BLOCK_FRAMES * 4];

		xbox_adpcm_decode_block(data + block * XBOX_ADPCM_BLOCK_BYTES * channels, channels, frames);
		for (unsigned int sample = 0; sample < XBOX_ADPCM_BLOCK_FRAMES * channels; sample++)
		{
			little[sample * 2] = (unsigned char)frames[sample];
			little[sample * 2 + 1] = (unsigned char)((unsigned short)frames[sample] >> 8);
		}
		crc = crc_add(crc, little, XBOX_ADPCM_BLOCK_FRAMES * channels * 2);
	}
	printf("DECODE frames=%lu crc=%08lx\n", blocks * XBOX_ADPCM_BLOCK_FRAMES, crc);
	free(data);
	return 0;
}

static int pack(const char *path, const char *name)
{
	unsigned long size, count;
	unsigned char *data = read_file(path, &size);
	struct wii_sound_pack_report report;
	int failures = 0;

	if (!data || size < 64)
		return 2;
	count = be32(data + 12);
	wii_sound_pack_set_verify(1);
	if (!wii_sound_pack_open(path, name))
	{
		printf("OPEN result=0\n");
		free(data);
		return 0;
	}
	for (unsigned long index = 0; index < count; index++)
	{
		const unsigned char *entry = data + 64 + count * 4 + index * 24;
		unsigned long offset = be32(entry), bytes = be32(entry + 4);
		unsigned char *buffer = malloc(bytes ? bytes : 1);
		int result = wii_sound_pack_read((long)offset, (long)bytes, buffer);

		printf("READ offset=%08lx size=%lu result=%d crc=%08lx\n", offset, bytes, result,
			result > 0 ? crc_add(0, buffer, bytes) : 0UL);
		failures += result != 1;
		/* refused: another size, an offset with no entry */
		if (index == 0)
		{
			printf("READ offset=%08lx size=%lu result=%d\n", offset, bytes + 1,
				wii_sound_pack_read((long)offset, (long)bytes + 1, buffer));
			printf("READ offset=%08lx size=%lu result=%d\n", offset + 1, bytes,
				wii_sound_pack_read((long)offset + 1, (long)bytes, buffer));
		}
		free(buffer);
	}
	wii_sound_pack_report_get(&report);
	printf("PACK entries=%lu reads=%lu bytes=%lu failures=%lu decode_checks=%lu read_failures=%d\n", report.entries,
		report.reads, report.bytes, report.failures, report.decode_checks, failures);
	wii_sound_pack_close();
	free(data);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "decode"))
		return decode((unsigned int)atoi(argv[2]), argv[3]);
	if (argc == 4 && !strcmp(argv[1], "pack"))
		return pack(argv[2], argv[3]);
	fprintf(stderr, "usage: sound_check decode <channels> <file> | pack <pack> <map name>\n");
	return 2;
}
