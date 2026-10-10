#include "semantics.h"
#include "bitfield_boundary.h"
#include "calls.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Inputs travel through volatile objects so the compiler cannot evaluate the
   operation at build time on the build machine. */
static volatile long long volatile_q = -7000000000000LL;
static volatile unsigned long long volatile_u = 0xfedcba9876543210ULL;
static volatile int volatile_i = 16777217;
static volatile unsigned volatile_umax = 0xffffffffu;
static volatile unsigned volatile_uodd = 0x80000001u;
static volatile float volatile_3e9 = 3e9f;
static volatile float volatile_zero_f = 0.0f;
static volatile double volatile_zero_d = 0.0;

#define F32(bits) wii_abi_f32_from_bits(bits)
#define F64(bits) wii_abi_f64_from_bits(bits)

static void target(struct wii_abi_report *report)
{
    wii_abi_observe_u64(report, "target.pointer_bytes", sizeof(void *));
    wii_abi_observe_u64(report, "target.flt_eval_method", (uint64_t)(int64_t)FLT_EVAL_METHOD);
    wii_abi_check_u64(report, "type.short_bytes", WII_ABI_CONTRACT, sizeof(short), 2);
    wii_abi_check_u64(report, "type.int_bytes", WII_ABI_CONTRACT, sizeof(int), 4);
    wii_abi_check_u64(report, "type.long_long_bytes", WII_ABI_CONTRACT, sizeof(long long), 8);
    struct with_double { char c; double d; };
    struct with_int64 { char c; long long q; };
    wii_abi_check_u64(report, "layout.double_after_char", WII_ABI_ASSUMPTION, offsetof(struct with_double, d), 8);
    wii_abi_check_u64(report, "layout.int64_after_char", WII_ABI_ASSUMPTION, offsetof(struct with_int64, q), 8);
    wii_abi_check_u64(report, "layout.mixed_struct_bytes", WII_ABI_ASSUMPTION, sizeof(struct wii_abi_mixed), 12);
    wii_abi_check_u64(report, "layout.byte3_struct_bytes", WII_ABI_ASSUMPTION, sizeof(struct wii_abi_byte3), 3);
}

static void int64_checks(struct wii_abi_report *report)
{
    long long q = volatile_q;
    unsigned long long u = volatile_u;
    wii_abi_check_i64(report, "int64.signed_divide", WII_ABI_CONTRACT, q / 3, -2333333333333LL);
    wii_abi_check_i64(report, "int64.signed_remainder", WII_ABI_CONTRACT, q % 3, -1);
    wii_abi_check_u64(report, "int64.unsigned_divide", WII_ABI_CONTRACT, u / 0x12345u, 0xe0004fa01c4dULL);
    wii_abi_check_u64(report, "int64.unsigned_remainder", WII_ABI_CONTRACT, u % 0x12345u, 0x10a4fULL);
    wii_abi_check_u64(report, "int64.multiply_wraps", WII_ABI_CONTRACT,
                      (unsigned long long)(u >> 28) * 0x987654321ULL, (0xfedcba987ULL * 0x987654321ULL));
    wii_abi_check_u64(report, "int64.unsigned_shift", WII_ABI_CONTRACT, u >> 60, 0xf);
    wii_abi_check_i64(report, "int64.negative_right_shift", WII_ABI_ASSUMPTION, q >> 40, -7);
}

static void conversion_checks(struct wii_abi_report *report)
{
    wii_abi_check_u64(report, "convert.int_to_float_tie", WII_ABI_CONTRACT, wii_abi_f32_bits((float)volatile_i), 0x4b800000u);
    wii_abi_check_u64(report, "convert.u32max_to_float", WII_ABI_CONTRACT, wii_abi_f32_bits((float)volatile_umax), 0x4f800000u);
    wii_abi_check_u64(report, "convert.u32odd_to_float", WII_ABI_CONTRACT, wii_abi_f32_bits((float)volatile_uodd), 0x4f000000u);
    wii_abi_check_u64(report, "convert.float_to_u32", WII_ABI_CONTRACT, (unsigned)volatile_3e9, 3000000000u);
    wii_abi_check_u64(report, "convert.int64max_to_double", WII_ABI_CONTRACT,
                      wii_abi_f64_bits((double)(volatile_q * 0 + 0x7fffffffffffffffLL)), 0x43e0000000000000ULL);
    static const struct { uint64_t in; uint32_t out; const char *id; } narrowing[] = {
        {0x3ff0000010000000ULL, 0x3f800000u, "convert.double_to_float.tie_even_down"},
        {0x3ff0000030000000ULL, 0x3f800002u, "convert.double_to_float.tie_even_up"},
        {0x36a0000000000000ULL, 0x00000001u, "convert.double_to_float.min_subnormal"},
        {0x3690000000000000ULL, 0x00000000u, "convert.double_to_float.half_min_subnormal_tie"},
        {0x3698000000000000ULL, 0x00000001u, "convert.double_to_float.three_quarter_subnormal"},
        {0x47efffffffffffffULL, 0x7f800000u, "convert.double_to_float.overflow_to_inf"}};
    for (size_t i = 0; i < sizeof(narrowing) / sizeof(narrowing[0]); ++i) {
        volatile double in = F64(narrowing[i].in);
        wii_abi_check_u64(report, narrowing[i].id, WII_ABI_CONTRACT, wii_abi_f32_bits((float)in), narrowing[i].out);
    }
}

static void subnormal_checks(struct wii_abi_report *report)
{
    volatile float min_normal = FLT_MIN, smallest = F32(1), three = F32(3), half = 0.5f, two = 2.0f;
    wii_abi_check_u64(report, "float.subnormal.half_min_normal", WII_ABI_CONTRACT,
                      wii_abi_f32_bits(min_normal * half), 0x00400000u);
    wii_abi_check_u64(report, "float.subnormal.double_smallest", WII_ABI_CONTRACT,
                      wii_abi_f32_bits(smallest * two), 0x00000002u);
    wii_abi_check_u64(report, "float.subnormal.half_three_tie_even", WII_ABI_CONTRACT,
                      wii_abi_f32_bits(three * half), 0x00000002u);
    volatile float scale30 = 1073741824.0f;
    wii_abi_check_u64(report, "float.subnormal.input_scaled_to_normal", WII_ABI_CONTRACT,
                      wii_abi_f32_bits(smallest * scale30), 0x04000000u);
    volatile double dsmallest = F64(1), scale60 = 1152921504606846976.0;
    wii_abi_check_u64(report, "double.subnormal.input_scaled_to_normal", WII_ABI_CONTRACT,
                      wii_abi_f64_bits(dsmallest * scale60), 0x0090000000000000ULL);
    volatile float sub = F32(0x00400000u);
    wii_abi_check_u64(report, "float.subnormal.sum_to_normal", WII_ABI_CONTRACT, wii_abi_f32_bits(sub + sub), 0x00800000u);
    volatile double dmin = DBL_MIN, dhalf = 0.5;
    wii_abi_check_u64(report, "double.subnormal.half_min_normal", WII_ABI_CONTRACT,
                      wii_abi_f64_bits(dmin * dhalf), 0x0008000000000000ULL);
    volatile float two_f = 2.0f;
    volatile double two_d = 2.0;
    wii_abi_check_u64(report, "sqrt.float_two", WII_ABI_CONTRACT, wii_abi_f32_bits(sqrtf(two_f)), 0x3fb504f3u);
    wii_abi_check_u64(report, "sqrt.double_two", WII_ABI_CONTRACT, wii_abi_f64_bits(sqrt(two_d)), 0x3ff6a09e667f3bcdULL);
    wii_abi_check_u64(report, "sqrt.float_min_subnormal", WII_ABI_CONTRACT, wii_abi_f32_bits(sqrtf(smallest)), 0x1a3504f3u);
    volatile double minus_half = -0.5, two_half = 2.5;
    wii_abi_check_u64(report, "floor.minus_half", WII_ABI_CONTRACT, wii_abi_f64_bits(floor(minus_half)), 0xbff0000000000000ULL);
    wii_abi_check_u64(report, "ceil.minus_half", WII_ABI_CONTRACT, wii_abi_f64_bits(ceil(minus_half)), 0x8000000000000000ULL);
    wii_abi_check_u64(report, "floor.two_half", WII_ABI_CONTRACT, wii_abi_f64_bits(floor(two_half)), 0x4000000000000000ULL);
    wii_abi_check_u64(report, "float.divide_by_zero", WII_ABI_CONTRACT, wii_abi_f32_bits(1.0f / volatile_zero_f), 0x7f800000u);
    /* A generated NaN's sign/payload is target-defined (x86 0xffc00000, PPC
       0x7fc00000): observation only. */
    wii_abi_observe_u64(report, "float.generated_nan_bits", wii_abi_f32_bits(volatile_zero_f / volatile_zero_f));
    wii_abi_observe_u64(report, "double.generated_nan_bits", wii_abi_f64_bits(volatile_zero_d / volatile_zero_d));
    volatile float snan = F32(0x7f812345u);
    volatile double widened = snan;
    wii_abi_observe_u64(report, "float.signaling_nan_widened_bits", wii_abi_f64_bits(widened));
}

static void vararg_checks(struct wii_abi_report *report)
{
    static const float plane[4] = {0.0f, 0.0f, 1.0f, 2.0f};
    static const float center[2] = {3.0f, 4.0f};
    static const float color[4] = {1.0f, 0.5f, 0.25f, 0.125f};
    static const uint32_t radii[] = {0x3fc00000u, 0x80000000u, 0x00000001u, 0x7f7fffffu, 0x7fc12345u, 0xff800000u};
    char id[96];
    for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); ++i) {
        struct wii_abi_circle_record record;
        memset(&record, 0, sizeof(record));
        float radius = F32(radii[i]);
        float offset = F32(radii[(i + 1) % (sizeof(radii) / sizeof(radii[0]))]);
        short projection = (short)(-7 + (int)i);
        unsigned char sign = (unsigned char)(i & 1);
        wii_abi_vararg_circle(&record, 4, plane, projection, sign, center, radius, color, offset);
        int ok = record.plane == plane && record.projection == projection && record.sign == sign &&
                 record.center == center && record.color == color;
        snprintf(id, sizeof(id), "varargs.render_debug_circle.%zu.fields", i);
        wii_abi_check_u64(report, id, WII_ABI_CONTRACT, ok, 1);
        snprintf(id, sizeof(id), "varargs.render_debug_circle.%zu.radius_bits", i);
        wii_abi_check_u64(report, id, WII_ABI_CONTRACT, wii_abi_f32_bits(record.radius), radii[i]);
        snprintf(id, sizeof(id), "varargs.render_debug_circle.%zu.offset_bits", i);
        wii_abi_check_u64(report, id, WII_ABI_CONTRACT, wii_abi_f32_bits(record.offset), wii_abi_f32_bits(offset));
    }

    uint64_t got[24];
    int marker = 0;
    const uint64_t expected[] = {
        (uint64_t)(int64_t)-1, 0x0123456789abcdefULL, wii_abi_f64_bits(1.25), (uint64_t)(int64_t)7,
        (uint64_t)(int64_t)-9000000000000LL, wii_abi_f64_bits(-0.0), (uint64_t)(uintptr_t)&marker,
        (uint64_t)(int64_t)-2147483647L, (uint64_t)(int64_t)3, 0x8000000000000000ULL, wii_abi_f64_bits(1e300),
        (uint64_t)(int64_t)11, (uint64_t)(int64_t)12, (uint64_t)(int64_t)13, 0x7fffffffffffffffULL,
        wii_abi_f64_bits(5e-324), wii_abi_f64_bits(3.0f), (uint64_t)(int64_t)-14, 0x1122334455667788ULL,
        wii_abi_f64_bits(-2.5)};
    int count = wii_abi_vararg_mixed(got, 24, "iLdiLdpliLdiiiLddiLd",
                                     -1, 0x0123456789abcdefLL, 1.25, 7, -9000000000000LL, -0.0, (void *)&marker,
                                     -2147483647L, 3, (long long)0x8000000000000000ULL, 1e300, 11, 12, 13,
                                     0x7fffffffffffffffLL, 5e-324, 3.0f, -14, 0x1122334455667788LL, -2.5);
    wii_abi_check_i64(report, "varargs.mixed.count", WII_ABI_CONTRACT, count, 20);
    for (int i = 0; i < 20 && i < count; ++i) {
        snprintf(id, sizeof(id), "varargs.mixed.%02d", i);
        wii_abi_check_u64(report, id, WII_ABI_CONTRACT, got[i], expected[i]);
    }
    wii_abi_check_i64(report, "varargs.va_copy_twice", WII_ABI_CONTRACT,
                      wii_abi_vararg_copy_twice(5, 1, -2, 3, -4, 5), ((((1L * 31 - 2) * 31 + 3) * 31 - 4) * 31 + 5));
}

static void format_checks(struct wii_abi_report *report)
{
    char buffer[128];
    wii_abi_format(buffer, sizeof(buffer), "%d|%ld|%u|%x|%08lX", -42, -1234567L, 4000000000u, 0xbeefu, 0x1234abcdUL);
    wii_abi_check_text(report, "format.integers", WII_ABI_CONTRACT, buffer, "-42|-1234567|4000000000|beef|1234ABCD");
    wii_abi_format(buffer, sizeof(buffer), "%s|%-6s|%c|%%|%lld", "abc", "de", 'Z', -9000000000000LL);
    wii_abi_check_text(report, "format.strings", WII_ABI_CONTRACT, buffer, "abc|de    |Z|%|-9000000000000");
    float tenth = 0.1f;
    wii_abi_format(buffer, sizeof(buffer), "%f|%.9g|%5.1f|%.3f", tenth, tenth, 3.14159, -2.0005f);
    wii_abi_check_text(report, "format.floats", WII_ABI_CONTRACT, buffer, "0.100000|0.100000001|  3.1|-2.000");
    /* Exact decimal halves, exponent spelling and %g: rounding and spelling
       are C-library behaviour; compared between targets. */
    wii_abi_format(buffer, sizeof(buffer), "%.2f|%.0f|%.0f|%.1f|%e|%g|%g", 0.125, 0.5, 2.5, 0.25, 1.5, 0.0001, 1e-5);
    wii_abi_observe_bytes(report, "format.halves_exponent", (const uint8_t *)buffer, strlen(buffer));
    int written = wii_abi_variadic_pointer()(buffer, 4, "%d-%s", 1234, "xyz");
    wii_abi_check_i64(report, "format.pointer_truncated_length", WII_ABI_CONTRACT, written, 8);
    wii_abi_check_text(report, "format.pointer_truncated_text", WII_ABI_CONTRACT, buffer, "123");
}

static float caller_sub(float a, float b) { return a - b * 0.5f; }

static int compare_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

static void aggregate_checks(struct wii_abi_report *report)
{
    struct wii_abi_point3 a = {1.0f, 2.0f, 3.0f}, b = {0.5f, -0.25f, 4.0f};
    struct wii_abi_point3 sum = wii_abi_point_add(a, b);
    wii_abi_check_u64(report, "struct.point3_return", WII_ABI_CONTRACT,
                      (uint64_t)wii_abi_f32_bits(sum.x) << 32 ^ (uint64_t)wii_abi_f32_bits(sum.y) << 16 ^
                      wii_abi_f32_bits(sum.z), (uint64_t)0x3fc00000u << 32 ^ (uint64_t)0x3fe00000u << 16 ^ 0x40e00000u);
    struct wii_abi_quaternion q = {1.0f, -2.0f, 0.5f, 4.0f};
    struct wii_abi_quaternion scaled = wii_abi_quaternion_scale(q, -0.5f);
    wii_abi_check_u64(report, "struct.quaternion_return", WII_ABI_CONTRACT,
                      scaled.i == -0.5f && scaled.j == 1.0f && scaled.k == -0.25f && scaled.w == -2.0f, 1);
    struct wii_abi_short_pair pair = {-32768, 32767};
    struct wii_abi_short_pair swapped = wii_abi_short_pair_swap(pair);
    wii_abi_check_u64(report, "struct.short_pair_return", WII_ABI_CONTRACT, swapped.x == 32767 && swapped.y == -32768, 1);
    struct wii_abi_mixed mixed = {(char)-3, 1.25f, 100};
    struct wii_abi_mixed bumped = wii_abi_mixed_bump(mixed, 5);
    wii_abi_check_u64(report, "struct.mixed_return", WII_ABI_CONTRACT,
                      bumped.c == 2 && bumped.r == 2.5f && bumped.s == 95, 1);
    struct wii_abi_matrix matrix;
    matrix.scale = 3.0f;
    for (int i = 0; i < 12; ++i) matrix.m[i] = (float)(i - 6) * 0.25f;
    struct wii_abi_matrix negated = wii_abi_matrix_negate(matrix);
    int matrix_ok = negated.scale == 1.5f && matrix.scale == 3.0f;
    for (int i = 0; i < 12; ++i) matrix_ok &= negated.m[i] == -matrix.m[i];
    wii_abi_check_u64(report, "struct.matrix4x3_by_value", WII_ABI_CONTRACT, matrix_ok, 1);
    struct wii_abi_byte3 bytes = {{1, 2, 3}};
    struct wii_abi_byte3 rotated = wii_abi_byte3_rotate(bytes);
    wii_abi_check_u64(report, "struct.byte3_return", WII_ABI_CONTRACT,
                      rotated.b[0] == 2 && rotated.b[1] == 3 && rotated.b[2] == 1, 1);

    uint64_t echo[26];
    struct wii_abi_point3 point = {-1.0f, 0.0f, 65504.0f};
    wii_abi_many(echo, 1, 1.5, -2LL, 3, 0.25f, 4, -8.0, 0x100000000LL, 5, -0.5f, 6, 1e-310, 7, 3.0f, 8, -0.0, 9,
                 F32(0x00000001u), 10, 2.0, 11, 1e30f, -3LL, point);
    const uint64_t expected[26] = {
        1, wii_abi_f64_bits(1.5), (uint64_t)-2LL, 3, wii_abi_f32_bits(0.25f), 4, wii_abi_f64_bits(-8.0), 0x100000000ULL,
        5, wii_abi_f32_bits(-0.5f), 6, wii_abi_f64_bits(1e-310), 7, wii_abi_f32_bits(3.0f), 8, wii_abi_f64_bits(-0.0),
        9, 1, 10, wii_abi_f64_bits(2.0), 11, wii_abi_f32_bits(1e30f), (uint64_t)-3LL, wii_abi_f32_bits(-1.0f), 0,
        wii_abi_f32_bits(65504.0f)};
    char id[64];
    for (int i = 0; i < 26; ++i) {
        snprintf(id, sizeof(id), "call.many_arguments.%02d", i);
        wii_abi_check_u64(report, id, WII_ABI_CONTRACT, echo[i], expected[i]);
    }

    size_t count = 0;
    const wii_abi_cell_proc *procs = wii_abi_cell_procs(&count);
    volatile long cell_inputs[3] = {5, 0x12348001L, -1};
    const long cell_expected[3] = {-5, -32767, -1};
    wii_abi_check_u64(report, "callback.cell_table_count", WII_ABI_CONTRACT, count, 3);
    for (size_t i = 0; i < 3 && i < count; ++i) {
        snprintf(id, sizeof(id), "callback.cell_table.%zu", i);
        wii_abi_check_i64(report, id, WII_ABI_CONTRACT, procs[i](cell_inputs[i]), cell_expected[i]);
    }
    volatile wii_abi_thread_proc thread = wii_abi_thread_entry;
    unsigned long thread_input = 0x12345678ul;
    wii_abi_check_u64(report, "callback.thread_entry", WII_ABI_CONTRACT, thread(&thread_input), 0xb791f3ddul);
    wii_abi_check_u64(report, "callback.real_both_directions", WII_ABI_CONTRACT,
                      wii_abi_f32_bits(wii_abi_apply(caller_sub, 3.0f, 2.0f)), wii_abi_f32_bits(2.5f));
    long values[16];
    uint32_t state = 0x9e3779b9u;
    for (int i = 0; i < 16; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        values[i] = (long)(int32_t)state;
    }
    qsort(values, 16, sizeof(values[0]), compare_long);
    int sorted = 1;
    for (int i = 1; i < 16; ++i) sorted &= values[i - 1] < values[i];
    wii_abi_check_u64(report, "callback.libc_qsort_sorted", WII_ABI_CONTRACT, sorted, 1);
    long key = values[11];
    const long *found = bsearch(&key, values, 16, sizeof(values[0]), compare_long);
    wii_abi_check_i64(report, "callback.libc_bsearch_index", WII_ABI_CONTRACT, found ? found - values : -1, 11);
}

static void helper_checks(struct wii_abi_report *report)
{
    struct wii_animation_event_header header, sentinel;
    memset(&sentinel, 0x5a, sizeof(sentinel));
    static const uint8_t one[] = {0x0d}, byte_delta[] = {0x0e, 0x05}, word_delta[] = {0x0f, 0x34, 0x12};
    static const uint8_t low_byte[] = {0x0e, 0x01}, low_word[] = {0x0f, 0xff, 0x00};
    int status = wii_decode_animation_event_header(one, 1, &header);
    wii_abi_check_u64(report, "helper.animation.kind1", WII_ABI_CANDIDATE,
                      status == WII_BITFIELD_OK && header.time_delta_kind == 1 && header.event_type == 3 &&
                      header.time_delta == 1 && header.header_size == 1, 1);
    status = wii_decode_animation_event_header(byte_delta, 2, &header);
    wii_abi_check_u64(report, "helper.animation.kind2", WII_ABI_CANDIDATE,
                      status == WII_BITFIELD_OK && header.time_delta_kind == 2 && header.event_type == 3 &&
                      header.time_delta == 5 && header.header_size == 2, 1);
    status = wii_decode_animation_event_header(word_delta, 3, &header);
    wii_abi_check_u64(report, "helper.animation.kind3_le16", WII_ABI_CANDIDATE,
                      status == WII_BITFIELD_OK && header.time_delta_kind == 3 && header.event_type == 3 &&
                      header.time_delta == 0x1234 && header.header_size == 3, 1);
    static const struct { const uint8_t *stream; size_t available; int status; const char *id; } rejects[] = {
        {one, 0, WII_BITFIELD_TRUNCATED, "helper.animation.reject_empty"},
        {byte_delta, 1, WII_BITFIELD_TRUNCATED, "helper.animation.reject_byte_truncated"},
        {word_delta, 2, WII_BITFIELD_TRUNCATED, "helper.animation.reject_word_truncated"},
        {word_delta, 1, WII_BITFIELD_TRUNCATED, "helper.animation.reject_word_header_only"},
        {low_byte, 2, WII_BITFIELD_RANGE, "helper.animation.reject_byte_delta_le1"},
        {low_word, 3, WII_BITFIELD_RANGE, "helper.animation.reject_word_delta_le255"},
        {NULL, 3, WII_BITFIELD_NULL, "helper.animation.reject_null"}};
    for (size_t i = 0; i < sizeof(rejects) / sizeof(rejects[0]); ++i) {
        header = sentinel;
        status = wii_decode_animation_event_header(rejects[i].stream, rejects[i].available, &header);
        wii_abi_check_u64(report, rejects[i].id, WII_ABI_CANDIDATE,
                          status == rejects[i].status && memcmp(&header, &sentinel, sizeof(header)) == 0, 1);
    }
    wii_abi_check_u64(report, "helper.animation.reject_null_output", WII_ABI_CANDIDATE,
                      wii_decode_animation_event_header(one, 1, NULL), WII_BITFIELD_NULL);
    /* every first byte at every availability 0..3 with tail 05 01 */
    unsigned long mismatches = 0;
    for (unsigned b = 0; b < 256; ++b) {
        const uint8_t stream[3] = {(uint8_t)b, 0x05, 0x01};
        for (size_t available = 0; available <= 3; ++available) {
            unsigned kind = b & 3u;
            size_t need = kind < 2 ? 1 : kind == 2 ? 2 : 3;
            header = sentinel;
            status = wii_decode_animation_event_header(stream, available, &header);
            if (available < need) {
                mismatches += status != WII_BITFIELD_TRUNCATED || memcmp(&header, &sentinel, sizeof(header)) != 0;
            } else {
                unsigned delta = kind < 2 ? kind : kind == 2 ? 5u : 0x105u;
                mismatches += status != WII_BITFIELD_OK || header.time_delta_kind != kind ||
                              header.event_type != (b >> 2) || header.time_delta != delta || header.header_size != need;
            }
        }
    }
    wii_abi_check_u64(report, "helper.animation.exhaustive_first_byte_x_available", WII_ABI_CANDIDATE, mismatches, 0);

    struct wii_message_header message;
    const uint8_t wire[2] = {0x34, 0x12};
    uint8_t encoded[2] = {0, 0};
    status = wii_decode_message_header(wire, 2, &message);
    wii_abi_check_u64(report, "helper.message.decode", WII_ABI_CANDIDATE,
                      status == WII_BITFIELD_OK && message.flags == 0 && message.type == 1 && message.size == 0x123, 1);
    wii_abi_check_u64(report, "helper.message.reject_truncated", WII_ABI_CANDIDATE,
                      wii_decode_message_header(wire, 1, &message), WII_BITFIELD_TRUNCATED);
    struct wii_message_header bad = {4, 0, 0};
    wii_abi_check_u64(report, "helper.message.reject_flags_range", WII_ABI_CANDIDATE,
                      wii_encode_message_header(&bad, encoded, 2), WII_BITFIELD_RANGE);
    bad.flags = 0; bad.size = 0x1000;
    wii_abi_check_u64(report, "helper.message.reject_size_range", WII_ABI_CANDIDATE,
                      wii_encode_message_header(&bad, encoded, 2), WII_BITFIELD_RANGE);
    bad.size = 1;
    wii_abi_check_u64(report, "helper.message.reject_capacity", WII_ABI_CANDIDATE,
                      wii_encode_message_header(&bad, encoded, 1), WII_BITFIELD_TRUNCATED);
    wii_abi_check_u64(report, "helper.message.untouched_after_rejects", WII_ABI_CANDIDATE, encoded[0] | encoded[1], 0);
    mismatches = 0;
    for (unsigned value = 0; value < 0x10000u; ++value) {
        const uint8_t in[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
        uint8_t out[2] = {0, 0};
        mismatches += wii_decode_message_header(in, 2, &message) != WII_BITFIELD_OK ||
                      message.flags != (value & 3u) || message.type != ((value >> 2) & 3u) || message.size != (value >> 4) ||
                      wii_encode_message_header(&message, out, 2) != WII_BITFIELD_OK || memcmp(in, out, 2) != 0;
    }
    wii_abi_check_u64(report, "helper.message.exhaustive_roundtrip", WII_ABI_CANDIDATE, mismatches, 0);

    uint8_t state = 0;
    int triggered = 0;
    unsigned ticks = 0;
    wii_abi_check_u64(report, "helper.bsp_switch.pack_none_12", WII_ABI_CANDIDATE,
                      wii_pack_bsp_switch_state(-1, 12, &state) == WII_BITFIELD_OK && state == 0xcf, 1);
    wii_abi_check_u64(report, "helper.bsp_switch.pack_2_12", WII_ABI_CANDIDATE,
                      wii_pack_bsp_switch_state(2, 12, &state) == WII_BITFIELD_OK && state == 0xc2, 1);
    state = 0xcf;
    wii_abi_check_u64(report, "helper.bsp_switch.unpack_cf", WII_ABI_CANDIDATE,
                      wii_unpack_bsp_switch_state(&state, &triggered, &ticks) == WII_BITFIELD_OK &&
                      triggered == -1 && ticks == 12, 1);
    state = 0x77;
    wii_abi_check_u64(report, "helper.bsp_switch.reject_triggered_8", WII_ABI_CANDIDATE,
                      wii_pack_bsp_switch_state(8, 0, &state) == WII_BITFIELD_RANGE && state == 0x77, 1);
    wii_abi_check_u64(report, "helper.bsp_switch.reject_triggered_minus9", WII_ABI_CANDIDATE,
                      wii_pack_bsp_switch_state(-9, 0, &state) == WII_BITFIELD_RANGE && state == 0x77, 1);
    wii_abi_check_u64(report, "helper.bsp_switch.reject_ticks_16", WII_ABI_CANDIDATE,
                      wii_pack_bsp_switch_state(0, 16, &state) == WII_BITFIELD_RANGE && state == 0x77, 1);
    wii_abi_check_u64(report, "helper.bsp_switch.reject_null", WII_ABI_CANDIDATE,
                      wii_unpack_bsp_switch_state(&state, NULL, &ticks), WII_BITFIELD_NULL);
    mismatches = 0;
    for (unsigned value = 0; value < 256; ++value) {
        uint8_t in = (uint8_t)value, out = 0;
        mismatches += wii_unpack_bsp_switch_state(&in, &triggered, &ticks) != WII_BITFIELD_OK ||
                      wii_pack_bsp_switch_state(triggered, ticks, &out) != WII_BITFIELD_OK || out != in;
    }
    wii_abi_check_u64(report, "helper.bsp_switch.exhaustive_roundtrip", WII_ABI_CANDIDATE, mismatches, 0);
}

void wii_abi_semantics(struct wii_abi_report *report)
{
    target(report);
    int64_checks(report);
    conversion_checks(report);
    subnormal_checks(report);
    vararg_checks(report);
    format_checks(report);
    aggregate_checks(report);
    helper_checks(report);
}
