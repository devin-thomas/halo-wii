/*
SOUND_PACK.C

HWI-074: a map's sound samples on the card (wii_sound.h). The Xbox reads a
permutation's samples from the map file at their offset into the sound
cache (source/cache/xbox_sound_cache.c, cache_file_read); the Wii's staged
map (cache_files_wii.c) holds no samples, so the cache layer asks here for
the bytes at that offset. tools/wii/sound_stage.py packs the map's
permutations, each the converter's HWS1 container (tools/wii/content_convert.py:
the Xbox ADPCM kept as it is, PCM big-endian), into d:\maps\<name>.wsnd:

  header, 64 bytes, big-endian:
    0  "HWSPACK1"
    8  version (1)
   12  entries
   16  CRC-32 of the offsets table
   20  CRC-32 of the entry table
   24  the file's bytes
   28  the map's name, 16 bytes, NUL-padded
   44  the converter generation, 16 hexadecimal digits
   60  CRC-32 of bytes 0..59
  offsets: entries x u32, each permutation's sample offset in the map,
    ascending (the cache's key);
  entries: entries x 24 bytes, in the same order:
    the sample offset, the samples' bytes in the map, the HWS1 container's
    offset and bytes in this file, the CRC-32 of the samples as the map
    holds them, the codec (1 Xbox ADPCM, 2 PCM16 big-endian) and 0;
  the containers, each at a 32-byte boundary.

A read is served only as the map holds it: the container's header must
agree with its entry, the bytes are restored to the map's (PCM to
little-endian) and must match the entry's CRC; anything else is refused and
reported, and the sound does not load (as an Xbox read that failed).
With verification on, each read's decoded PCM is logged (SOUND_DECODE: its
CRC-32 as little-endian 16-bit), for tools/wii/sound_stage.py's host
reference to compare.
*/

#include "wii_platform.h"
#include "wii_sound.h"
#include "xbox_adpcm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACK_HEADER_BYTES 64
#define PACK_ENTRY_BYTES 24
#define PACK_MAXIMUM_ENTRIES 65536
#define HWS_HEADER_BYTES 32

static struct
{
	FILE *file;
	char name[32];
	unsigned long count;
	unsigned long *offsets;
	unsigned long entries_at;
	unsigned long file_bytes;
	int verify;
	unsigned long errors_reported;
	struct wii_sound_pack_report report;
} pack;

static unsigned long pack_crc_table[256];

static void pack_crc_initialize(void)
{
	unsigned long index;

	if (pack_crc_table[1])
		return;
	for (index = 0; index < 256; index++)
	{
		unsigned long value = index;
		int bit;

		for (bit = 0; bit < 8; bit++)
			value = value & 1 ? 0xEDB88320UL ^ (value >> 1) : value >> 1;
		pack_crc_table[index] = value;
	}
}

static unsigned long pack_crc(unsigned long crc, const unsigned char *bytes, unsigned long count)
{
	crc = ~crc & 0xFFFFFFFFUL;
	while (count--)
		crc = pack_crc_table[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

static unsigned long be32(const unsigned char *bytes)
{
	return ((unsigned long)bytes[0] << 24) | ((unsigned long)bytes[1] << 16) | ((unsigned long)bytes[2] << 8) |
		bytes[3];
}

static unsigned int be16(const unsigned char *bytes)
{
	return ((unsigned int)bytes[0] << 8) | bytes[1];
}

static int pack_read_at(unsigned long offset, void *buffer, unsigned long bytes)
{
	return fseek(pack.file, (long)offset, SEEK_SET) == 0 && fread(buffer, 1, bytes, pack.file) == bytes;
}

static void pack_error(const char *what)
{
	pack.report.failures++;
	if (pack.errors_reported++ < 8)
		wii_log("SOUND_ERROR pack=%s text=\"%s\"\n", pack.name, what);
}

void wii_sound_pack_set_verify(int verify)
{
	pack.verify = verify;
}

void wii_sound_pack_report_get(struct wii_sound_pack_report *report)
{
	*report = pack.report;
	report->open = pack.file != NULL;
	report->entries = pack.count;
}

void wii_sound_pack_close(void)
{
	if (pack.file)
		fclose(pack.file);
	free(pack.offsets);
	pack.file = NULL;
	pack.offsets = NULL;
	pack.count = 0;
}

int wii_sound_pack_open(const char *path, const char *map_name)
{
	unsigned char header[PACK_HEADER_BYTES];
	unsigned char *table = NULL;
	unsigned long index;
	unsigned long long started = wii_time_microseconds();

	wii_sound_pack_close();
	pack_crc_initialize();
	memset(&pack.report, 0, sizeof(pack.report));
	pack.errors_reported = 0;
	snprintf(pack.name, sizeof(pack.name), "%s", map_name ? map_name : "");
	pack.file = fopen(path, "rb");
	if (!pack.file)
	{
		wii_log("SOUND_PACK map=%s open=0 reason=\"not on the card: stage it with tools/wii/sound_stage.py\"\n",
			pack.name);
		return 0;
	}
	if (!pack_read_at(0, header, sizeof(header)) || memcmp(header, "HWSPACK1", 8) || be32(header + 8) != 1 ||
		be32(header + 60) != pack_crc(0, header, 60) || strncmp((const char *)header + 28, pack.name, 16))
	{
		wii_log("SOUND_PACK map=%s open=0 reason=\"damaged or another map's: stage it again\"\n", pack.name);
		wii_sound_pack_close();
		return 0;
	}
	pack.count = be32(header + 12);
	pack.file_bytes = be32(header + 24);
	if (!pack.count || pack.count > PACK_MAXIMUM_ENTRIES)
	{
		wii_log("SOUND_PACK map=%s open=0 reason=\"%lu entries\"\n", pack.name, pack.count);
		wii_sound_pack_close();
		return 0;
	}
	table = malloc(pack.count * 4);
	pack.offsets = malloc(pack.count * sizeof(unsigned long));
	if (!table || !pack.offsets || !pack_read_at(PACK_HEADER_BYTES, table, pack.count * 4) ||
		pack_crc(0, table, pack.count * 4) != be32(header + 16))
	{
		free(table);
		wii_log("SOUND_PACK map=%s open=0 reason=\"its offsets table is damaged: stage it again\"\n", pack.name);
		wii_sound_pack_close();
		return 0;
	}
	for (index = 0; index < pack.count; index++)
		pack.offsets[index] = be32(table + index * 4);
	free(table);
	pack.entries_at = PACK_HEADER_BYTES + pack.count * 4;
	wii_log("SOUND_PACK map=%s open=1 entries=%lu bytes=%lu generation=%.16s us=%llu\n", pack.name, pack.count,
		pack.file_bytes, (const char *)header + 44, wii_time_microseconds() - started);
	return 1;
}

/* the decoded PCM's CRC-32 (16-bit little-endian), as the host reference's */
static void pack_log_decode(unsigned long offset, unsigned int codec, unsigned int channels, const unsigned char *bytes,
	unsigned long size)
{
	unsigned long crc = 0, frames = 0;

	if (codec == 1)
	{
		unsigned long blocks = size / (XBOX_ADPCM_BLOCK_BYTES * channels);
		unsigned long block;

		for (block = 0; block < blocks; block++)
		{
			short decoded[XBOX_ADPCM_BLOCK_FRAMES * 2];
			unsigned char little[XBOX_ADPCM_BLOCK_FRAMES * 4];
			unsigned int sample;

			xbox_adpcm_decode_block(bytes + block * XBOX_ADPCM_BLOCK_BYTES * channels, channels, decoded);
			for (sample = 0; sample < XBOX_ADPCM_BLOCK_FRAMES * channels; sample++)
			{
				little[sample * 2] = (unsigned char)decoded[sample];
				little[sample * 2 + 1] = (unsigned char)((unsigned short)decoded[sample] >> 8);
			}
			crc = pack_crc(crc, little, XBOX_ADPCM_BLOCK_FRAMES * channels * 2);
		}
		frames = blocks * XBOX_ADPCM_BLOCK_FRAMES;
	}
	else
	{
		/* (PCM: the map's own little-endian bytes are the samples) */
		crc = pack_crc(0, bytes, size - size % (2 * channels));
		frames = size / (2 * channels);
	}
	pack.report.decode_checks++;
	wii_log("SOUND_DECODE map=%s offset=%08lx codec=%u channels=%u frames=%lu crc=%08lx\n", pack.name, offset, codec,
		channels, frames, crc);
}

int wii_sound_pack_read(long offset, long size, void *buffer)
{
	unsigned char entry[PACK_ENTRY_BYTES];
	unsigned char hws[HWS_HEADER_BYTES];
	unsigned long low = 0, high, at, container, container_bytes, sample_bytes, codec, channels;
	unsigned long long started;

	if (!pack.file || offset < 0 || size <= 0)
		return 0;
	/* the entry whose samples are at offset */
	high = pack.count;
	while (low < high)
	{
		unsigned long middle = low + (high - low) / 2;

		if (pack.offsets[middle] < (unsigned long)offset)
			low = middle + 1;
		else
			high = middle;
	}
	if (low >= pack.count || pack.offsets[low] != (unsigned long)offset)
		return 0;
	started = wii_time_microseconds();
	at = pack.entries_at + low * PACK_ENTRY_BYTES;
	if (!pack_read_at(at, entry, sizeof(entry)) || be32(entry) != (unsigned long)offset)
	{
		pack_error("an entry could not be read or is damaged; stage the map's sounds again");
		return -1;
	}
	sample_bytes = be32(entry + 4);
	container = be32(entry + 8);
	container_bytes = be32(entry + 12);
	codec = be16(entry + 20);
	if (sample_bytes != (unsigned long)size || container_bytes != HWS_HEADER_BYTES + sample_bytes ||
		container + container_bytes > pack.file_bytes || (codec != 1 && codec != 2))
	{
		pack_error("a permutation's entry does not match the map's read; stage the map's sounds again");
		return -1;
	}
	if (!pack_read_at(container, hws, sizeof(hws)) || memcmp(hws, "HWS1", 4) || be16(hws + 4) != 1 ||
		be16(hws + 6) != codec || be32(hws + 20) != sample_bytes)
	{
		pack_error("a permutation's HWS1 header is damaged; stage the map's sounds again");
		return -1;
	}
	channels = be16(hws + 12);
	if ((channels != 1 && channels != 2) || fread(buffer, 1, sample_bytes, pack.file) != sample_bytes)
	{
		pack_error("a permutation's samples could not be read; check the card");
		return -1;
	}
	if (codec == 2)
	{
		/* the map's PCM is little-endian; the container's big-endian */
		unsigned char *bytes = (unsigned char *)buffer;
		unsigned long index;

		for (index = 0; index + 1 < sample_bytes; index += 2)
		{
			unsigned char swap = bytes[index];

			bytes[index] = bytes[index + 1];
			bytes[index + 1] = swap;
		}
		pack.report.pcm_reads++;
	}
	if (pack_crc(0, (const unsigned char *)buffer, sample_bytes) != be32(entry + 16))
	{
		pack_error("a permutation's samples fail their CRC; stage the map's sounds again");
		return -1;
	}
	pack.report.reads++;
	pack.report.bytes += sample_bytes;
	pack.report.read_microseconds += wii_time_microseconds() - started;
	if (pack.verify)
		pack_log_decode((unsigned long)offset, (unsigned int)codec, (unsigned int)channels,
			(const unsigned char *)buffer, sample_bytes);
	return 1;
}
