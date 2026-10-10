/*
MESSAGE_ENCRYPTION.C

symbols in this file:
0006EE40 0050:
	_reversible_crypt (0000)
0006EE90 0090:
	_tea_encipher (0000)
0006EF20 0090:
	_tea_decipher (0000)
0006EFB0 0100:
	_message_encrypt (0000)
0006F0B0 0100:
	_message_decrypt (0000)
0025579C 0030:
	??_C@_0DA@EDCJMCDB@?$CI0?$DM?$DNflags?$CJ?5?$CG?$CG?5?$CI?$CIflags?$CJ?$DM?$DNMESSAGE_@ (0000)
002557CC 000e:
	??_C@_0O@IIKLGMGM@msgptr?5?$CG?$CG?5key?$AA@ (0000)
002557DC 0036:
	??_C@_0DG@HFDJMHM@c?3?2halo?2SOURCE?2bungie_net?2common@ (0000)
*/

/* ---------- headers */

#include "cseries/cseries.h"

#include "bungie_net/common/message_encryption.h"
#include "bungie_net/common/message_header.h"

/* ---------- constants */

enum
{
	MESSAGE_ENCRYPTED_FLAG = FLAG(0),
	TEA_ROUND_COUNT = 32,
	TEA_BLOCK_SIZE = 8,
};

/* ---------- macros */

/* ---------- structures */

/* port: the header word was read through a union of it and the bit-fields
flags : 2, type : 2, message_size : 12. Bit-field allocation is the
compiler's: MSVC and GCC on x86 allocate from the low bit, as
build_message_header's shifts encode, but PowerPC EABI allocates from the
high bit, where a 20-byte message read as 332 bytes and was rewritten 312
bytes past its end. The header is decoded with message_header.h's own shifts
and masks instead, which read the same bits on x86 (ADR-018). */

/* ---------- prototypes */

static void message_block_load(byte const *bytes, unsigned long block[2]);
static void message_block_store(unsigned long const block[2], byte *bytes);
static void message_key_bytes(unsigned long const key[2], byte key_bytes[2 * 4]);

/* ---------- globals */

/* ---------- public code */

void reversible_crypt(
	byte *data,
	long data_size,
	byte const *key,
	long key_size)
{
	long data_index;
	long key_index;
	long key_step;

	data_index = 0;
	key_index = 0;
	key_step = 1;
	while (data_index < data_size)
	{
		data[data_index] = ~(data[data_index] ^ key[key_index]);
		key_index += key_step;
		data_index++;
		if (key_index == key_size || key_index < 0)
		{
			key_step = -key_step;
			key_index += key_step;
		}
	}

	return;
}

void tea_encipher(
	unsigned long const input[2],
	unsigned long output[2],
	long const key[4])
{
	unsigned long value0;
	unsigned long value1;
	long sum;
	unsigned long rounds;
	long key0;
	long key1;
	long key2;
	long key3;

	value0 = input[0];
	value1 = input[1];
	sum = 0;
	rounds = TEA_ROUND_COUNT;
	key0 = key[0];
	key1 = key[1];
	key2 = key[2];
	key3 = key[3];
	do
	{
		sum += 0x9E3779B9;
		value0 += ((value1 >> 5) + key1) ^ (value1 * 0x10 + key0) ^ (value1 + sum);
		value1 += ((value0 >> 5) + key3) ^ (value0 * 0x10 + key2) ^ (value0 + sum);
	}
	while (--rounds);
	output[0] = value0;
	output[1] = value1;

	return;
}

void tea_decipher(
	unsigned long const input[2],
	unsigned long output[2],
	long const key[4])
{
	unsigned long value0;
	unsigned long value1;
	long sum;
	unsigned long rounds;
	long key0;
	long key1;
	long key2;
	long key3;

	value0 = input[0];
	value1 = input[1];
	sum = 0xC6EF3720;
	rounds = TEA_ROUND_COUNT;
	key0 = key[0];
	key1 = key[1];
	key2 = key[2];
	key3 = key[3];
	do
	{
		value1 -= ((value0 >> 5) + key3) ^ (value0 * 0x10 + key2) ^ (value0 + sum);
		value0 -= ((value1 >> 5) + key1) ^ (value1 * 0x10 + key0) ^ (value1 + sum);
		sum -= 0x9E3779B9;
	}
	while (--rounds);
	output[0] = value0;
	output[1] = value1;

	return;
}

void message_encrypt(
	word *msgptr,
	unsigned long const key[2])
{
	word message_size;
	word flags;
	word block_count;
	short remainder_size;
	word *cursor;
	long key_copy[4];
	unsigned long block_index;
	unsigned long block[2];
	byte key_bytes[2 * 4];

	match_assert("c:\\halo\\SOURCE\\bungie_net\\common\\message_encryption.c", 31, msgptr && key);
	/* port: explicit header decode (see structures) */
	flags = GET_MESSAGE_FLAGS(*msgptr);
	message_size = GET_MESSAGE_SIZE(*msgptr);
	/* port: a size smaller than the header itself would wrap the block count
	below to 0xFFFF blocks; such a message is left as it is */
	if (message_size < sizeof(word))
		return;
	if (!TEST_FLAG(flags, 0))
	{
		block_count = (message_size - sizeof(word)) >> 3;
		remainder_size = (message_size - sizeof(word)) & 7;
		cursor = msgptr + 1;
		key_copy[0] = key_copy[2] = key[0];
		key_copy[1] = key_copy[3] = key[1];
		if (block_count)
		{
			block_index = block_count;
			do
			{
				/* port: through little-endian words (message_block_load) */
				message_block_load((byte const *)cursor, block);
				tea_encipher(block, block, key_copy);
				message_block_store(block, (byte *)cursor);
				cursor += TEA_BLOCK_SIZE / sizeof(word);
			}
			while (--block_index);
		}
		if (remainder_size)
		{
			/* port: the key's little-endian bytes (message_block_load) */
			message_key_bytes(key, key_bytes);
			reversible_crypt((byte *)cursor, (short)remainder_size, key_bytes, sizeof(key_bytes));
		}
		flags |= MESSAGE_ENCRYPTED_FLAG;
		match_assert("c:\\halo\\SOURCE\\bungie_net\\common\\message_encryption.c", 76, (0<=flags) && ((flags)<=MESSAGE_FLAG_BITS_MASK));
		*msgptr = (*msgptr & ~MESSAGE_FLAG_BITS_MASK) | flags;
	}

	return;
}

void message_decrypt(
	word *msgptr,
	unsigned long const key[2])
{
	word message_size;
	word flags;
	word block_count;
	short remainder_size;
	word *cursor;
	long key_copy[4];
	unsigned long block_index;
	unsigned long block[2];
	byte key_bytes[2 * 4];

	match_assert("c:\\halo\\SOURCE\\bungie_net\\common\\message_encryption.c", 88, msgptr && key);
	/* port: explicit header decode (see structures) */
	flags = GET_MESSAGE_FLAGS(*msgptr);
	message_size = GET_MESSAGE_SIZE(*msgptr);
	/* port: a size smaller than the header itself would wrap the block count
	below to 0xFFFF blocks; such a message is left as it is */
	if (message_size < sizeof(word))
		return;
	if (TEST_FLAG(flags, 0))
	{
		block_count = (message_size - sizeof(word)) >> 3;
		remainder_size = (message_size - sizeof(word)) & 7;
		cursor = msgptr + 1;
		key_copy[0] = key_copy[2] = key[0];
		key_copy[1] = key_copy[3] = key[1];
		if (block_count)
		{
			block_index = block_count;
			do
			{
				/* port: through little-endian words (message_block_load) */
				message_block_load((byte const *)cursor, block);
				tea_decipher(block, block, key_copy);
				message_block_store(block, (byte *)cursor);
				cursor += TEA_BLOCK_SIZE / sizeof(word);
			}
			while (--block_index);
		}
		if (remainder_size)
		{
			/* port: the key's little-endian bytes (message_block_load) */
			message_key_bytes(key, key_bytes);
			reversible_crypt((byte *)cursor, (short)remainder_size, key_bytes, sizeof(key_bytes));
		}
		flags &= ~MESSAGE_ENCRYPTED_FLAG;
		match_assert("c:\\halo\\SOURCE\\bungie_net\\common\\message_encryption.c", 131, (0<=flags) && ((flags)<=MESSAGE_FLAG_BITS_MASK));
		*msgptr = (*msgptr & ~MESSAGE_FLAG_BITS_MASK) | flags;
	}

	return;
}

/* ---------- private code */

/* port: an encrypted block is two 32-bit words, and the bytes the remainder is
crypted with are the key's: the Xbox loads both little-endian. Loading them
explicitly gives the same ciphertext on a big-endian target, and reads no
misaligned words (the blocks start two bytes into the message). On the x86
ports these are the values the casts loaded (ADR-018). */
static void message_block_load(
	byte const *bytes,
	unsigned long block[2])
{
	long index;

	for (index = 0; index < 2; index++, bytes += 4)
	{
		block[index] = (unsigned long)bytes[0] |
			((unsigned long)bytes[1] << 8) |
			((unsigned long)bytes[2] << 16) |
			((unsigned long)bytes[3] << 24);
	}

	return;
}

static void message_block_store(
	unsigned long const block[2],
	byte *bytes)
{
	long index;

	for (index = 0; index < 2; index++, bytes += 4)
	{
		bytes[0] = (byte)(block[index] & 0xFF);
		bytes[1] = (byte)((block[index] >> 8) & 0xFF);
		bytes[2] = (byte)((block[index] >> 16) & 0xFF);
		bytes[3] = (byte)((block[index] >> 24) & 0xFF);
	}

	return;
}

static void message_key_bytes(
	unsigned long const key[2],
	byte key_bytes[2 * 4])
{
	message_block_store(key, key_bytes);

	return;
}
