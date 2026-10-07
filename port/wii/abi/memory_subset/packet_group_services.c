#include "packet_shim.h"

/* The actual group descriptor contains one byte and no multi-byte fields.
 * This deliberately narrow authored service does not qualify the engine's
 * general descriptor walker or its pointer-sized metadata assumptions. */
void byte_swap_data(struct byte_swap_definition *definition, void *data, long count)
{
    match_assert(__FILE__, __LINE__, definition && data && count == 1);
    match_assert(__FILE__, __LINE__, definition->signature == BYTE_SWAP_DEFINITION_SIGNATURE);
    match_assert(__FILE__, __LINE__, definition->size == 1 && definition->codes);
    match_assert(__FILE__, __LINE__, definition->codes[0] == _begin_bs_array &&
                 definition->codes[1] == 1 && definition->codes[2] == _1byte &&
                 definition->codes[3] == _end_bs_array);
    definition->verified = TRUE;
}
