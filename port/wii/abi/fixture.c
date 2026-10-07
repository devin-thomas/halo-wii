/* Authored synthetic records. No Halo data, native-structure file casts or packing. */
#include "boundary.h"
#include "fixture.h"
#include <float.h>
#include <inttypes.h>
#include <stdarg.h>

struct sample { uint32_t address; uint16_t index; float weight; };
struct result { uint32_t integer; double real; };
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24, "Requires IEEE binary32");

static struct result signature(uint32_t integer, double real, float extra)
{
    struct result result = { integer ^ UINT32_C(0x55aa55aa), real + extra };
    return result;
}

static int variadic(const char *unused, ...)
{
    (void)unused;
    va_list args;
    va_start(args, unused);
    int integer = va_arg(args, int);
    double real = va_arg(args, double);
    uint64_t wide = va_arg(args, uint64_t);
    va_end(args);
    return integer == -7 && real == 1.5 && wide == UINT64_C(0x1122334455667788);
}

/* Model the engine's first-byte/word store without executing its uninitialized
 * long or aliasing UB. Only the fixed one/two-byte synthetic cases call this. */
static uint32_t model_first_store(uint32_t payload, uint16_t value,
                                  unsigned int bytes, bool big_endian)
{
    uint8_t cell[4];
    for (unsigned int i = 0; i < 4; ++i) {
        unsigned int shift = 8 * (big_endian ? 3 - i : i);
        cell[i] = (uint8_t)(payload >> shift);
    }
    for (unsigned int i = 0; i < bytes; ++i) {
        unsigned int shift = 8 * (big_endian ? bytes - 1 - i : i);
        cell[i] = (uint8_t)(value >> shift);
    }
    uint32_t result = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        unsigned int shift = 8 * (big_endian ? 3 - i : i);
        result |= (uint32_t)cell[i] << shift;
    }
    return result;
}

int wii_abi_report(FILE *output)
{
    unsigned int checks = 0;
#define REQUIRE(condition, name) do { ++checks; if (!(condition)) { \
    fprintf(output, "ABI FAIL %s check=%u\n", name, checks); return 1; } } while (0)
    const uint8_t wire[] = {0xee, 0x10, 0x60, 0x3a, 0x80, 0x00, 0x00, 0xc0, 0x3f};
    uint32_t address = 0, bits = 0, offset = UINT32_MAX;
    REQUIRE(wii_read_le32(wire, sizeof(wire), 1, &address) && address == UINT32_C(0x803a6010), "unaligned_address");
    REQUIRE(wii_read_le32(wire, sizeof(wire), 5, &bits) && wii_float_from_bits(bits) == 1.5f, "float_bits");
    REQUIRE(wii_xbox_span(UINT32_C(0x803a6000), 64, address, 4, &offset) && offset == 16, "encoded_span");
    REQUIRE(!wii_xbox_span(UINT32_C(0x803a6000), 64, UINT32_C(0x803a5fff), 4, &offset), "address_below_base");
    REQUIRE(!wii_xbox_span(UINT32_C(0x803a6000), 64, UINT32_C(0x803a603f), 4, &offset), "span_truncated");
    REQUIRE(!wii_xbox_span(UINT32_C(0xfffffff0), 64, UINT32_C(0xfffffff1), 4, &offset), "region_overflow");
    REQUIRE(wii_array_span(64, 16, 12, 4), "array_exact_end");
    REQUIRE(!wii_array_span(64, 16, UINT32_MAX, 4), "count_overflow");
    REQUIRE(!wii_array_span(64, UINT32_MAX, 1, 4), "offset_overflow");
    REQUIRE(!wii_array_span(64, 0, 1, 0), "zero_element");
    REQUIRE(!wii_read_le32(wire, sizeof(wire), 6, &bits), "wire_truncated");
    REQUIRE(!wii_read_le32(wire, sizeof(wire), SIZE_MAX, &bits), "wire_offset_overflow");
    REQUIRE(offsetof(struct sample, address) == 0 && offsetof(struct sample, index) == 4 &&
            offsetof(struct sample, weight) == 8 && sizeof(struct sample) == 12, "native_layout");
    struct result (*volatile call)(uint32_t, double, float) = signature;
    struct result returned = call(UINT32_C(0x11223344), 1.0, 0.5f);
    REQUIRE(returned.integer == UINT32_C(0x448866ee) && returned.real == 1.5, "function_struct_return");
    REQUIRE(variadic("fixture", -7, 1.5f, UINT64_C(0x1122334455667788)), "varargs_promotions");
    REQUIRE(wii_float_bits(-0.0f) == UINT32_C(0x80000000), "signed_zero");
    uint32_t datum = UINT32_C(0xabcd1234);
    REQUIRE((uint16_t)datum == UINT16_C(0x1234) && (uint16_t)(datum >> 16) == UINT16_C(0xabcd), "datum_halves");
    const uint32_t payload = UINT32_C(0xa1b2c3d4);
    REQUIRE(model_first_store(payload, 1, 1, false) == UINT32_C(0xa1b2c301) &&
            model_first_store(payload, 1, 1, true) == UINT32_C(0x01b2c3d4), "first_byte_endian_failure_model");
    REQUIRE(model_first_store(payload, UINT16_C(0x1234), 2, false) == UINT32_C(0xa1b21234) &&
            model_first_store(payload, UINT16_C(0x1234), 2, true) == UINT32_C(0x1234c3d4), "first_word_endian_failure_model");
    /* Existing HS conversions use n == 0; retain that behavior in this model. */
    REQUIRE(wii_cell_replace_low8(payload, 0 == 0) == UINT32_C(0xa1b2c301), "hs_zero_boolean_payload");
    REQUIRE(wii_cell_replace_low8(payload, -7 == 0) == UINT32_C(0xa1b2c300), "hs_nonzero_boolean_payload");
    REQUIRE(wii_cell_replace_low16(payload, (uint16_t)-2) == UINT32_C(0xa1b2fffe), "hs_negative_short_payload");
    REQUIRE(wii_cell_replace_low16(payload, UINT16_C(0x1234)) == UINT32_C(0xa1b21234), "hs_positive_short_payload");
    REQUIRE(wii_cell_replace_low16(payload, (uint16_t)(int16_t)-2.75f) == UINT32_C(0xa1b2fffe), "hs_real_short_payload");
    volatile float factor = wii_float_from_bits(UINT32_C(0x3f800001));
    volatile float subtract = wii_float_from_bits(UINT32_C(0x3f800002));
    float separated = factor * factor - subtract;
    volatile float third = 1.0f / 3.0f;
    float sum = 0.0f;
    for (unsigned int i = 0; i < 1000; ++i) sum += third;
    if (fprintf(output, "ABI CANONICAL checks=%u address=803a6010 offset=16 float=3fc00000 layout=0,4,8/12 varargs=ok datum=abcd,1234 cells=a1b2c301,a1b2fffe\n",
                checks) < 0 ||
        fprintf(output, "ABI ARITHMETIC separate=%08" PRIx32 " sum1000=%08" PRIx32 "\n",
                wii_float_bits(separated), wii_float_bits(sum)) < 0 ||
        fprintf(output, "ABI TARGET pointer_bytes=%u int_bytes=%u double_bytes=%u\n",
                (unsigned int)sizeof(void *), (unsigned int)sizeof(int), (unsigned int)sizeof(double)) < 0) return 1;
    return 0;
#undef REQUIRE
}

#ifdef WII_ABI_HOST
int main(void)
{
    int result = wii_abi_report(stdout);
    if (fflush(stdout) != 0) return 1;
    return result;
}
#endif
