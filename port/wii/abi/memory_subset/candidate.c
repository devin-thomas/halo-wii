#include "candidate.h"

static long checked_width(struct data_encoding_state *state, short count, long size)
{
    long width;
    match_assert(__FILE__, __LINE__, state && state->buffer &&
                 state->buffer_size >= 0 && state->offset >= 0 &&
                 state->offset <= state->buffer_size);
    switch (size) {
    case 1: width = 1; break;
    case -2: width = 2; break;
    case -4: width = 4; break;
    case -8: width = 8; break;
    default: state->overflow = TRUE; return 0;
    }
    /* Subtraction/division rejects before either size or cursor can overflow. */
    if (state->overflow || count < 0 ||
        count > (state->buffer_size - state->offset) / width) {
        state->overflow = TRUE;
        return 0;
    }
    return width;
}

boolean candidate_encode_memory(struct data_encoding_state *state, const void *source,
                                short count, long size)
{
    match_assert(__FILE__, __LINE__, state && state->offset < state->buffer_size);
    long width = checked_width(state, count, size);
    if (!width) return FALSE;
    byte *destination = state->buffer + state->offset;
    const byte *input = source;
    for (short n = 0; n < count; ++n) {
        uint64_t value = 0;
        if (input) {
            switch (width) {
            case 1: value = *input; break;
            case 2: { uint16_t v; memcpy(&v, input, 2); value = v; break; }
            case 4: { uint32_t v; memcpy(&v, input, 4); value = v; break; }
            case 8: memcpy(&value, input, 8); break;
            }
            input += width;
        }
        for (long i = 0; i < width; ++i)
            destination[i] = (byte)(value >> (8 * (width - i - 1)));
        destination += width;
    }
    state->offset += (long)count * width;
    return TRUE;
}

void *candidate_decode_memory(struct data_encoding_state *state, short count, long size)
{
    long width = checked_width(state, count, size);
    if (!width) return NULL;
    byte *memory = state->buffer + state->offset;
    byte *input = memory;
    for (short n = 0; n < count; ++n) {
        uint64_t value = 0;
        for (long i = 0; i < width; ++i) value = (value << 8) | input[i];
        switch (width) {
        case 1: break;
        case 2: { uint16_t v = (uint16_t)value; memcpy(input, &v, 2); break; }
        case 4: { uint32_t v = (uint32_t)value; memcpy(input, &v, 4); break; }
        case 8: memcpy(input, &value, 8); break;
        }
        input += width;
    }
    state->offset += (long)count * width;
    return memory;
}

boolean candidate_encode_integer(struct data_encoding_state *state, long value, long maximum)
{
    match_assert(__FILE__, __LINE__, maximum > 0);
    if (maximum <= UNSIGNED_CHAR_MAX) {
        byte v = (byte)value;
        return candidate_encode_memory(state, &v, 1, 1);
    }
    if (maximum <= UNSIGNED_SHORT_MAX) {
        short v = (short)value;
        return candidate_encode_memory(state, &v, 1, -2);
    }
    return candidate_encode_memory(state, &value, 1, -4);
}

byte candidate_decode_byte(struct data_encoding_state *state)
{
    byte value = 0;
    void *memory = candidate_decode_memory(state, 1, 1);
    if (memory) memcpy(&value, memory, 1);
    return value;
}

short candidate_decode_short(struct data_encoding_state *state)
{
    short value = 0;
    void *memory = candidate_decode_memory(state, 1, -2);
    if (memory) memcpy(&value, memory, 2);
    return value;
}

long candidate_decode_long(struct data_encoding_state *state)
{
    long value = 0;
    void *memory = candidate_decode_memory(state, 1, -4);
    if (memory) memcpy(&value, memory, 4);
    return value;
}

__int64 candidate_decode_int64(struct data_encoding_state *state)
{
    __int64 value = 0;
    void *memory = candidate_decode_memory(state, 1, -8);
    if (memory) memcpy(&value, memory, 8);
    return value;
}

long candidate_decode_integer(struct data_encoding_state *state, long maximum)
{
    match_assert(__FILE__, __LINE__, maximum > 0);
    if (maximum <= UNSIGNED_CHAR_MAX) return candidate_decode_byte(state);
    if (maximum <= UNSIGNED_SHORT_MAX) return candidate_decode_short(state);
    return candidate_decode_long(state);
}
