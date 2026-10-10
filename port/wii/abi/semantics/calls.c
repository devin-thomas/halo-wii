#include "calls.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void wii_abi_vararg_circle(struct wii_abi_circle_record *out, int kind, ...)
{
    va_list list;
    va_start(list, kind);
    out->plane = va_arg(list, const float *);
    out->projection = (short)va_arg(list, int);
    out->sign = (unsigned char)va_arg(list, int);
    out->center = va_arg(list, const float *);
    out->radius = (float)va_arg(list, double);
    out->color = va_arg(list, const float *);
    out->offset = (float)va_arg(list, double);
    va_end(list);
}

int wii_abi_vararg_mixed(uint64_t *out, size_t capacity, const char *types, ...)
{
    va_list list;
    size_t index = 0;
    va_start(list, types);
    for (; types[index] != '\0'; ++index) {
        if (index >= capacity) { va_end(list); return -1; }
        uint64_t bits = 0;
        switch (types[index]) {
        case 'i': bits = (uint64_t)(int64_t)va_arg(list, int); break;
        case 'l': bits = (uint64_t)(int64_t)va_arg(list, long); break;
        case 'L': bits = (uint64_t)va_arg(list, long long); break;
        case 'd': { double value = va_arg(list, double); memcpy(&bits, &value, sizeof(bits)); break; }
        case 'p': bits = (uint64_t)(uintptr_t)va_arg(list, void *); break;
        default: va_end(list); return -1;
        }
        out[index] = bits;
    }
    va_end(list);
    return (int)index;
}

long wii_abi_vararg_copy_twice(int count, ...)
{
    va_list list, copy;
    long first = 0, second = 0;
    va_start(list, count);
    va_copy(copy, list);
    for (int i = 0; i < count; ++i) first = first * 31 + va_arg(list, int);
    for (int i = 0; i < count; ++i) second = second * 31 + va_arg(copy, int);
    va_end(copy);
    va_end(list);
    return first == second ? first : -1;
}

int wii_abi_format(char *buffer, size_t size, const char *format, ...)
{
    va_list list;
    va_start(list, format);
    int result = vsnprintf(buffer, size, format, list);
    va_end(list);
    return result;
}

struct wii_abi_point3 wii_abi_point_add(struct wii_abi_point3 a, struct wii_abi_point3 b)
{
    struct wii_abi_point3 result = {a.x + b.x, a.y + b.y, a.z + b.z};
    return result;
}

struct wii_abi_quaternion wii_abi_quaternion_scale(struct wii_abi_quaternion q, float s)
{
    struct wii_abi_quaternion result = {q.i * s, q.j * s, q.k * s, q.w * s};
    return result;
}

struct wii_abi_short_pair wii_abi_short_pair_swap(struct wii_abi_short_pair p)
{
    struct wii_abi_short_pair result = {p.y, p.x};
    return result;
}

struct wii_abi_mixed wii_abi_mixed_bump(struct wii_abi_mixed m, int delta)
{
    m.c = (char)(m.c + delta);
    m.r = m.r * 2.0f;
    m.s = (short)(m.s - delta);
    return m;
}

struct wii_abi_matrix wii_abi_matrix_negate(struct wii_abi_matrix m)
{
    for (int i = 0; i < 12; ++i) m.m[i] = -m.m[i];
    m.scale = m.scale * 0.5f;
    return m;
}

struct wii_abi_byte3 wii_abi_byte3_rotate(struct wii_abi_byte3 b)
{
    struct wii_abi_byte3 result = {{b.b[1], b.b[2], b.b[0]}};
    return result;
}

static uint64_t f64(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t f32(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void wii_abi_many(uint64_t *echo,
                  int a0, double f0, long long q0, int a1, float f1, int a2, double f2, long long q1,
                  int a3, float f3, int a4, double f4, int a5, float f5, int a6, double f6,
                  int a7, float f7, int a8, double f8, int a9, float f9, long long q2,
                  struct wii_abi_point3 p)
{
    uint64_t values[] = {
        (uint64_t)(int64_t)a0, f64(f0), (uint64_t)q0, (uint64_t)(int64_t)a1, f32(f1), (uint64_t)(int64_t)a2,
        f64(f2), (uint64_t)q1, (uint64_t)(int64_t)a3, f32(f3), (uint64_t)(int64_t)a4, f64(f4),
        (uint64_t)(int64_t)a5, f32(f5), (uint64_t)(int64_t)a6, f64(f6), (uint64_t)(int64_t)a7, f32(f7),
        (uint64_t)(int64_t)a8, f64(f8), (uint64_t)(int64_t)a9, f32(f9), (uint64_t)q2,
        f32(p.x), f32(p.y), f32(p.z)};
    memcpy(echo, values, sizeof(values));
}

static long cell_negate(long value) { return -value; }
static long cell_low16(long value) { return (long)(short)(value & 0xffff); }
static long cell_identity(long value) { return value; }

static const wii_abi_cell_proc cell_procs[] = {cell_negate, cell_low16, cell_identity};

const wii_abi_cell_proc *wii_abi_cell_procs(size_t *count)
{
    *count = sizeof(cell_procs) / sizeof(cell_procs[0]);
    return cell_procs;
}

unsigned long wii_abi_thread_entry(void *input)
{
    return *(const unsigned long *)input ^ 0xa5a5a5a5ul;
}

float wii_abi_apply(wii_abi_real_proc proc, float a, float b)
{
    return proc(a, b) + proc(b, a);
}

wii_abi_variadic_proc wii_abi_variadic_pointer(void)
{
    return wii_abi_format;
}
