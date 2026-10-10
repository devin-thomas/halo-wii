/* Built with the engine flags (it includes the actual real_math.h). Every
   input is produced with integer arithmetic plus exactly rounded IEEE
   operations, and digested, so a difference in a MATH line's inputs digest
   would expose a generator difference rather than a function difference. */
#include "abi_engine_types.h"
#include "math_corpus.h"
#include <stdint.h>
#include <string.h>

enum { RANDOM_CASES = 512, ENGINE_CASES = 512 };

struct corpus_rng { uint32_t state; };

static uint32_t next32(struct corpus_rng *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng->state = x;
    return x;
}

/* [0, 1) with 24 significant bits: exact in binary32 */
static float unit24(struct corpus_rng *rng)
{
    return (float)(next32(rng) >> 8) * (1.0f / 16777216.0f);
}

/* [0, 1) with 53 significant bits: exact in binary64 */
static double unit53(struct corpus_rng *rng)
{
    uint64_t high = next32(rng) >> 5, low = next32(rng) >> 6;
    return (double)(high * 67108864u + low) * (1.0 / 9007199254740992.0);
}

static float range_f(struct corpus_rng *rng, float lo, float hi)
{
    return lo + (hi - lo) * unit24(rng);
}

static uint64_t canonical_nan64(double value)
{
    return value != value ? UINT64_C(0x7ff8000000000000) : wii_abi_f64_bits(value);
}

static uint32_t canonical_nan32(float value)
{
    return value != value ? UINT32_C(0x7fc00000) : wii_abi_f32_bits(value);
}

struct unary_digests { struct wii_abi_digest raw64, raw32, canonical; };

static void unary_begin(struct unary_digests *d, unsigned long count)
{
    wii_abi_digest_begin(&d->raw64, count);
    wii_abi_digest_begin(&d->raw32, count);
    wii_abi_digest_begin(&d->canonical, count);
}

static void input64(struct unary_digests *d, double value)
{
    uint64_t bits = wii_abi_f64_bits(value);
    wii_abi_digest_input64(&d->raw64, bits);
    wii_abi_digest_input64(&d->raw32, bits);
    wii_abi_digest_input64(&d->canonical, bits);
}

/* binary32 inputs are digested as authored bits: widening a signalling NaN
   to double is target-defined (x86 quiets it, PPC lfs does not) */
static void input32(struct unary_digests *d, uint32_t bits)
{
    wii_abi_digest_input32(&d->raw64, bits);
    wii_abi_digest_input32(&d->raw32, bits);
    wii_abi_digest_input32(&d->canonical, bits);
}

static void unary_add(struct unary_digests *d, double result)
{
    float narrowed = (float)result;
    struct wii_abi_digest *all[3] = {&d->raw64, &d->raw32, &d->canonical};
    wii_abi_digest_result64(&d->raw64, wii_abi_f64_bits(result));
    wii_abi_digest_result32(&d->raw32, wii_abi_f32_bits(narrowed));
    wii_abi_digest_result64(&d->canonical, canonical_nan64(result));
    wii_abi_digest_result32(&d->canonical, canonical_nan32(narrowed));
    for (int i = 0; i < 3; ++i) wii_abi_digest_next(all[i]);
}

static void unary_emit(struct wii_abi_report *report, const char *name, const struct unary_digests *d)
{
    char id[80];
    snprintf(id, sizeof(id), "musl.%s.f64", name);
    wii_abi_digest_emit(report, id, &d->raw64);
    snprintf(id, sizeof(id), "musl.%s.narrowed_f32", name);
    wii_abi_digest_emit(report, id, &d->raw32);
    snprintf(id, sizeof(id), "musl.%s.nan_canonical", name);
    wii_abi_digest_emit(report, id, &d->canonical);
}

static const uint64_t specials[] = {
    UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000001),
    UINT64_C(0x8010000000000000), UINT64_C(0x3ff0000000000000), UINT64_C(0xbff0000000000000),
    UINT64_C(0x3fe0000000000000), UINT64_C(0x3ff921fb54442d18), UINT64_C(0x400921fb54442d18),
    UINT64_C(0x401921fb54442d18), UINT64_C(0x3ff921fb60000000), UINT64_C(0x7fefffffffffffff),
    UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000000),
    UINT64_C(0x4086232bdd7abcd2), UINT64_C(0xc0874910d52d3051), UINT64_C(0x3ee4f8b588e368f1),
    UINT64_C(0x4415af1d78b58c40), UINT64_C(0x3fefffffffffffff), UINT64_C(0x3ff0000000000001),
    UINT64_C(0x36a0000000000000), UINT64_C(0x47efffffe0000000), UINT64_C(0xc00921fb54442d18)};

typedef double (*unary_fn)(double);

static void musl_unary(struct wii_abi_report *report, const char *name, unary_fn fn, float lo, float hi, uint32_t seed)
{
    struct corpus_rng rng = {seed};
    const unsigned long specials_count = sizeof(specials) / sizeof(specials[0]);
    struct unary_digests d;
    unary_begin(&d, specials_count + 3 * RANDOM_CASES);
    for (unsigned long i = 0; i < specials_count; ++i) {
        double x = wii_abi_f64_from_bits(specials[i]);
        input64(&d, x);
        unary_add(&d, fn(x));
    }
    for (int i = 0; i < RANDOM_CASES; ++i) { /* game pattern: a real promoted to double */
        float x = range_f(&rng, lo, hi);
        input32(&d, wii_abi_f32_bits(x));
        unary_add(&d, fn(x));
    }
    for (int i = 0; i < RANDOM_CASES; ++i) {
        double x = (double)lo + ((double)hi - (double)lo) * unit53(&rng);
        input64(&d, x);
        unary_add(&d, fn(x));
    }
    for (int i = 0; i < RANDOM_CASES; ++i) { /* any binary32 bit pattern, NaN/Inf/subnormal included */
        uint32_t bits = next32(&rng);
        input32(&d, bits);
        unary_add(&d, fn(wii_abi_f32_from_bits(bits)));
    }
    unary_emit(report, name, &d);
}

typedef double (*binary_fn)(double, double);

static void musl_binary(struct wii_abi_report *report, const char *name, binary_fn fn,
                        float xlo, float xhi, float ylo, float yhi, uint32_t seed)
{
    struct corpus_rng rng = {seed};
    const unsigned long specials_count = sizeof(specials) / sizeof(specials[0]);
    struct unary_digests d;
    unary_begin(&d, specials_count * specials_count + 3 * RANDOM_CASES);
    for (unsigned long i = 0; i < specials_count; ++i)
        for (unsigned long j = 0; j < specials_count; ++j) {
            double x = wii_abi_f64_from_bits(specials[i]), y = wii_abi_f64_from_bits(specials[j]);
            input64(&d, x);
            input64(&d, y);
            unary_add(&d, fn(x, y));
        }
    for (int i = 0; i < RANDOM_CASES; ++i) {
        float x = range_f(&rng, xlo, xhi), y = range_f(&rng, ylo, yhi);
        input32(&d, wii_abi_f32_bits(x));
        input32(&d, wii_abi_f32_bits(y));
        unary_add(&d, fn(x, y));
    }
    for (int i = 0; i < RANDOM_CASES; ++i) {
        double x = (double)xlo + ((double)xhi - (double)xlo) * unit53(&rng);
        double y = (double)ylo + ((double)yhi - (double)ylo) * unit53(&rng);
        input64(&d, x);
        input64(&d, y);
        unary_add(&d, fn(x, y));
    }
    for (int i = 0; i < RANDOM_CASES; ++i) {
        uint32_t xbits = next32(&rng), ybits = next32(&rng);
        input32(&d, xbits);
        input32(&d, ybits);
        unary_add(&d, fn(wii_abi_f32_from_bits(xbits), wii_abi_f32_from_bits(ybits)));
    }
    unary_emit(report, name, &d);
}

/* ---------- engine routines */

static void vector_input(struct corpus_rng *rng, real_vector3d *v, float scale)
{
    v->i = range_f(rng, -scale, scale);
    v->j = range_f(rng, -scale, scale);
    v->k = range_f(rng, -scale, scale);
}

static void quaternion_input(struct corpus_rng *rng, real_quaternion *q)
{
    vector_input(rng, &q->v, 1.0f);
    q->w = range_f(rng, -1.0f, 1.0f);
}

static void add_reals(struct wii_abi_digest *d, const real *values, int count)
{
    for (int i = 0; i < count; ++i) wii_abi_digest_result32(d, wii_abi_f32_bits(values[i]));
}

static void add_inputs(struct wii_abi_digest *d, const real *values, int count)
{
    for (int i = 0; i < count; ++i) wii_abi_digest_input32(d, wii_abi_f32_bits(values[i]));
}

static void engine_vectors(struct wii_abi_report *report)
{
    struct corpus_rng rng = {0x1badb002u};
    struct wii_abi_digest magnitude, normalize, dot, cross, perpendicular, rotate, angle;
    struct wii_abi_digest *all[] = {&magnitude, &normalize, &dot, &cross, &perpendicular, &rotate, &angle};
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wii_abi_digest_begin(all[i], ENGINE_CASES);
    for (int c = 0; c < ENGINE_CASES; ++c) {
        real_vector3d a, b, result;
        float scale = c % 4 == 0 ? 0.0001f : c % 4 == 1 ? 1.0f : c % 4 == 2 ? 100.0f : 30000.0f;
        vector_input(&rng, &a, scale);
        vector_input(&rng, &b, 1.0f);
        real angle_input = range_f(&rng, -7.0f, 7.0f);
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
            add_inputs(all[i], a.n, 3);
            add_inputs(all[i], b.n, 3);
        }
        real m = magnitude3d(&a);
        add_reals(&magnitude, &m, 1);
        result = a;
        real n = normalize3d(&result);
        add_reals(&normalize, &n, 1);
        add_reals(&normalize, result.n, 3);
        real d = dot_product3d(&a, &b);
        add_reals(&dot, &d, 1);
        add_reals(&cross, cross_product3d(&a, &b, &result)->n, 3);
        add_reals(&perpendicular, perpendicular3d(&a, &result)->n, 3);
        real_vector3d axis = b;
        normalize3d(&axis);
        result = a;
        wii_abi_digest_input32(&rotate, wii_abi_f32_bits(angle_input));
        add_reals(&rotate, rotate_vector_about_axis(&result, &axis, sine(angle_input), cosine(angle_input))->n, 3);
        real between = angle_between_vectors3d(&a, &b);
        add_reals(&angle, &between, 1);
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wii_abi_digest_next(all[i]);
    }
    wii_abi_digest_emit(report, "engine.magnitude3d", &magnitude);
    wii_abi_digest_emit(report, "engine.normalize3d", &normalize);
    wii_abi_digest_emit(report, "engine.dot_product3d", &dot);
    wii_abi_digest_emit(report, "engine.cross_product3d", &cross);
    wii_abi_digest_emit(report, "engine.perpendicular3d", &perpendicular);
    wii_abi_digest_emit(report, "engine.rotate_vector_about_axis", &rotate);
    wii_abi_digest_emit(report, "engine.angle_between_vectors3d", &angle);
}

static void engine_rotations(struct wii_abi_report *report)
{
    struct corpus_rng rng = {0xc0ffee11u};
    struct wii_abi_digest multiply, interpolate, normalize, transform, to_matrix, to_quaternion, matrix_multiply,
        inverse, point, vector;
    struct wii_abi_digest *all[] = {&multiply, &interpolate, &normalize, &transform, &to_matrix, &to_quaternion,
                                    &matrix_multiply, &inverse, &point, &vector};
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wii_abi_digest_begin(all[i], ENGINE_CASES);
    for (int c = 0; c < ENGINE_CASES; ++c) {
        real_quaternion q0, q1, qr;
        real_point3d p, pr;
        real_vector3d v, vr;
        real_matrix4x3 m0, m1, mr;
        quaternion_input(&rng, &q0);
        quaternion_input(&rng, &q1);
        p.x = range_f(&rng, -500.0f, 500.0f); p.y = range_f(&rng, -500.0f, 500.0f); p.z = range_f(&rng, -500.0f, 500.0f);
        vector_input(&rng, &v, 10.0f);
        real t = unit24(&rng);
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
            add_inputs(all[i], q0.v.n, 3); add_inputs(all[i], &q0.w, 1);
            add_inputs(all[i], q1.v.n, 3); add_inputs(all[i], &q1.w, 1);
            add_inputs(all[i], p.n, 3); add_inputs(all[i], v.n, 3); add_inputs(all[i], &t, 1);
        }
        quaternions_multiply(&q0, &q1, &qr);
        add_reals(&multiply, qr.v.n, 3); add_reals(&multiply, &qr.w, 1);
        quaternions_interpolate(&q0, &q1, t, &qr);
        add_reals(&interpolate, qr.v.n, 3); add_reals(&interpolate, &qr.w, 1);
        qr = q0;
        quaternion_normalize(&qr);
        add_reals(&normalize, qr.v.n, 3); add_reals(&normalize, &qr.w, 1);
        quaternion_transform_point(&qr, &p, &pr);
        add_reals(&transform, pr.n, 3);
        matrix4x3_rotation_from_quaternion(&m0, &qr);
        m0.position = p;
        add_reals(&to_matrix, &m0.scale, 1); add_reals(&to_matrix, &m0.n[0][0], 12);
        matrix4x3_rotation_to_quaternion(&m0, &qr);
        add_reals(&to_quaternion, qr.v.n, 3); add_reals(&to_quaternion, &qr.w, 1);
        qr = q1;
        quaternion_normalize(&qr);
        matrix4x3_rotation_from_quaternion(&m1, &qr);
        m1.scale = 0.5f + t;
        m1.position.x = v.i; m1.position.y = v.j; m1.position.z = v.k;
        matrix4x3_multiply(&m0, &m1, &mr);
        add_reals(&matrix_multiply, &mr.scale, 1); add_reals(&matrix_multiply, &mr.n[0][0], 12);
        matrix4x3_inverse(&m1, &mr);
        add_reals(&inverse, &mr.scale, 1); add_reals(&inverse, &mr.n[0][0], 12);
        add_reals(&point, matrix4x3_transform_point(&m1, &p, &pr)->n, 3);
        add_reals(&vector, matrix4x3_transform_vector(&m1, &v, &vr)->n, 3);
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wii_abi_digest_next(all[i]);
    }
    wii_abi_digest_emit(report, "engine.quaternions_multiply", &multiply);
    wii_abi_digest_emit(report, "engine.quaternions_interpolate", &interpolate);
    wii_abi_digest_emit(report, "engine.quaternion_normalize", &normalize);
    wii_abi_digest_emit(report, "engine.quaternion_transform_point", &transform);
    wii_abi_digest_emit(report, "engine.matrix4x3_rotation_from_quaternion", &to_matrix);
    wii_abi_digest_emit(report, "engine.matrix4x3_rotation_to_quaternion", &to_quaternion);
    wii_abi_digest_emit(report, "engine.matrix4x3_multiply", &matrix_multiply);
    wii_abi_digest_emit(report, "engine.matrix4x3_inverse", &inverse);
    wii_abi_digest_emit(report, "engine.matrix4x3_transform_point", &point);
    wii_abi_digest_emit(report, "engine.matrix4x3_transform_vector", &vector);
}

static void engine_scalars(struct wii_abi_report *report)
{
    struct corpus_rng rng = {0x5eed1234u};
    struct wii_abi_digest trig, arctan, difference, root, ftol;
    struct wii_abi_digest *all[] = {&trig, &arctan, &difference, &root, &ftol};
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wii_abi_digest_begin(all[i], 4 * ENGINE_CASES);
    for (int c = 0; c < 4 * ENGINE_CASES; ++c) {
        real a = range_f(&rng, -20.0f, 20.0f), b = range_f(&rng, -20.0f, 20.0f);
        real r = c & 1 ? wii_abi_f32_from_bits(next32(&rng) & 0x7fffffffu) : range_f(&rng, 0.0f, 1e6f);
        real f = c < 2048 ? (real)(c - 1024) * 0.5f : range_f(&rng, -3e9f, 3e9f) / 1024.0f;
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
            add_inputs(all[i], &a, 1); add_inputs(all[i], &b, 1); add_inputs(all[i], &r, 1); add_inputs(all[i], &f, 1);
        }
        real values[3] = {sine(a), cosine(a), tangent(a)};
        add_reals(&trig, values, 3);
        values[0] = arctangent(a, b);
        add_reals(&arctan, values, 1);
        values[0] = signed_angular_difference(a, b);
        add_reals(&difference, values, 1);
        values[0] = square_root(r);
        add_reals(&root, values, 1);
        wii_abi_digest_result32(&ftol, (uint32_t)fast_ftol(f));
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); ++i) wii_abi_digest_next(all[i]);
    }
    wii_abi_digest_emit(report, "engine.sine_cosine_tangent", &trig);
    wii_abi_digest_emit(report, "engine.arctangent", &arctan);
    wii_abi_digest_emit(report, "engine.signed_angular_difference", &difference);
    wii_abi_digest_emit(report, "engine.square_root", &root);
    wii_abi_digest_emit(report, "engine.fast_ftol", &ftol);
}

static void engine_random(struct wii_abi_report *report)
{
    struct wii_abi_digest sequence, ranged, real_ranged;
    wii_abi_digest_begin(&sequence, 65536);
    wii_abi_digest_begin(&ranged, 65536);
    wii_abi_digest_begin(&real_ranged, 65536);
    unsigned long seed_a = 0, seed_b = 0xdeadbeeful, seed_c = 42;
    for (unsigned long i = 0; i < 65536; ++i) {
        wii_abi_digest_result32(&sequence, seed_random(&seed_a));
        wii_abi_digest_result32(&ranged, (uint32_t)(int32_t)seed_random_range(&seed_b, -300, 1200));
        wii_abi_digest_result32(&real_ranged, wii_abi_f32_bits(real_seed_random_range(&seed_c, -2.5f, 7.25f)));
        wii_abi_digest_next(&sequence); wii_abi_digest_next(&ranged); wii_abi_digest_next(&real_ranged);
    }
    wii_abi_digest_emit(report, "engine.seed_random", &sequence);
    wii_abi_digest_emit(report, "engine.seed_random_range", &ranged);
    wii_abi_digest_emit(report, "engine.real_seed_random_range", &real_ranged);
}

/* ---------- gameplay expressions (replicas of the cited lines) */

/* source/game/game_time.c game_time_update: leftover accumulation and
   floor(game_time*ticks_per_second) at 30 Hz, for three frame-time patterns */
static void gameplay_ticks(struct wii_abi_report *report)
{
    static const char *names[3] = {"gameplay.game_time_ticks.60hz", "gameplay.game_time_ticks.59_94hz",
                                   "gameplay.game_time_ticks.jitter"};
    for (int pattern = 0; pattern < 3; ++pattern) {
        struct corpus_rng rng = {0x30u + (uint32_t)pattern};
        struct wii_abi_digest d;
        wii_abi_digest_begin(&d, 20000);
        real leftover_dt = 0.f, speed = 1.f;
        real ticks_per_second = speed * TICKS_PER_SECOND;
        long total = 0;
        for (int frame = 0; frame < 20000; ++frame) {
            real time_delta_sec = pattern == 0 ? 1.0f / 60.0f : pattern == 1 ? 1001.0f / 60000.0f
                                                                         : range_f(&rng, 0.010f, 0.040f);
            real game_time = time_delta_sec + leftover_dt;
            real ticks_elapsed_real = (real)floor(game_time * ticks_per_second);
            long ticks_elapsed = (long)(ticks_elapsed_real <= (real)1000 ? ticks_elapsed_real : (real)1000);
            if (ticks_elapsed > 7) {
                ticks_elapsed = 7;
                game_time = ticks_elapsed_real / ticks_per_second;
            }
            leftover_dt = game_time - ticks_elapsed_real / ticks_per_second;
            if (leftover_dt < 0.f) leftover_dt = 0.f;
            total += ticks_elapsed;
            wii_abi_digest_input32(&d, wii_abi_f32_bits(time_delta_sec));
            wii_abi_digest_result32(&d, (uint32_t)ticks_elapsed);
            wii_abi_digest_result32(&d, wii_abi_f32_bits(leftover_dt));
            wii_abi_digest_next(&d);
        }
        wii_abi_digest_emit(report, names[pattern], &d);
        char id[80];
        snprintf(id, sizeof(id), "%s.total", names[pattern]);
        wii_abi_observe_u64(report, id, (uint64_t)total);
    }
}

/* source/game/players.c: starting_location_rating *= (real)pow((double)real_random_range(0, 1), 0.5)
   and the best-rating comparison; source/game/players.c vehicle flip test up.k > (real)cos(angle) */
static void gameplay_decisions(struct wii_abi_report *report)
{
    struct wii_abi_digest spawn, flip;
    wii_abi_digest_begin(&spawn, 4096);
    wii_abi_digest_begin(&flip, 4096);
    unsigned long seed = 0x600dcafeul;
    struct corpus_rng rng = {0xf11b0000u};
    for (int round = 0; round < 4096; ++round) {
        real best = 0.0f;
        long best_index = NONE;
        for (long location = 0; location < 8; ++location) {
            real rating = (real)(location + 1) * 0.125f;
            rating *= (real)pow((double)real_seed_random_range(&seed, 0.0f, 1.0f), 0.5);
            wii_abi_digest_result32(&spawn, wii_abi_f32_bits(rating));
            if (rating > best) { best = rating; best_index = location; }
        }
        wii_abi_digest_result32(&spawn, (uint32_t)best_index);
        wii_abi_digest_next(&spawn);
        real angle = range_f(&rng, 0.0f, 3.2f), up_k = range_f(&rng, -1.0f, 1.0f);
        wii_abi_digest_input32(&flip, wii_abi_f32_bits(angle));
        wii_abi_digest_input32(&flip, wii_abi_f32_bits(up_k));
        real threshold = (real)cos(angle);
        wii_abi_digest_result32(&flip, wii_abi_f32_bits(threshold));
        wii_abi_digest_result32(&flip, up_k > threshold);
        wii_abi_digest_next(&flip);
    }
    wii_abi_digest_emit(report, "gameplay.spawn_rating_pow", &spawn);
    wii_abi_digest_emit(report, "gameplay.vehicle_flip_cos", &flip);
}

void wii_abi_math_corpus(struct wii_abi_report *report)
{
    musl_unary(report, "sin", halo_sin, -25.2f, 25.2f, 0x00000101u);
    musl_unary(report, "cos", halo_cos, -25.2f, 25.2f, 0x00000202u);
    musl_unary(report, "tan", halo_tan, -25.2f, 25.2f, 0x00000303u);
    musl_unary(report, "asin", halo_asin, -1.0001f, 1.0001f, 0x00000404u);
    musl_unary(report, "acos", halo_acos, -1.0001f, 1.0001f, 0x00000505u);
    musl_unary(report, "atan", halo_atan, -200.0f, 200.0f, 0x00000606u);
    musl_unary(report, "exp", halo_exp, -110.0f, 110.0f, 0x00000707u);
    musl_unary(report, "log", halo_log, 0.0f, 1e6f, 0x00000808u);
    musl_unary(report, "log2", halo_log2, 0.0f, 1e6f, 0x00000909u);
    musl_unary(report, "log10", halo_log10, 0.0f, 1e6f, 0x00000a0au);
    musl_binary(report, "atan2", halo_atan2, -10.0f, 10.0f, -10.0f, 10.0f, 0x00000b0bu);
    musl_binary(report, "pow", halo_pow, 0.0f, 12.0f, -9.0f, 9.0f, 0x00000c0cu);
    engine_vectors(report);
    engine_rotations(report);
    engine_scalars(report);
    engine_random(report);
    gameplay_ticks(report);
    gameplay_decisions(report);
}
