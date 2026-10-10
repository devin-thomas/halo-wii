#ifndef WII_ABI_SEMANTICS_CALLS_H
#define WII_ABI_SEMANTICS_CALLS_H
/* Callee translation unit for calling-convention checks. Shapes mirror engine
   types (real_point3d, real_quaternion, point2d, real_matrix4x3) without their
   headers so that every call crosses a real object-file boundary. */

#include <stddef.h>
#include <stdint.h>

struct wii_abi_point3 { float x, y, z; };
struct wii_abi_quaternion { float i, j, k, w; };
struct wii_abi_short_pair { short x, y; };
struct wii_abi_mixed { char c; float r; short s; };
struct wii_abi_matrix { float scale; float m[12]; };
struct wii_abi_byte3 { unsigned char b[3]; };

/* source/render/render_debug.c (render_debug_add_*): pointers, short/boolean
   read as int, real read as double and narrowed. */
struct wii_abi_circle_record
{
    const float *plane;
    short projection;
    unsigned char sign;
    const float *center;
    float radius;
    const float *color;
    float offset;
};
void wii_abi_vararg_circle(struct wii_abi_circle_record *out, int kind, ...);
/* types: i=int l=long L=long long d=double p=pointer; raw bits stored in out */
int wii_abi_vararg_mixed(uint64_t *out, size_t capacity, const char *types, ...);
long wii_abi_vararg_copy_twice(int count, ...);
int wii_abi_format(char *buffer, size_t size, const char *format, ...);

struct wii_abi_point3 wii_abi_point_add(struct wii_abi_point3 a, struct wii_abi_point3 b);
struct wii_abi_quaternion wii_abi_quaternion_scale(struct wii_abi_quaternion q, float s);
struct wii_abi_short_pair wii_abi_short_pair_swap(struct wii_abi_short_pair p);
struct wii_abi_mixed wii_abi_mixed_bump(struct wii_abi_mixed m, int delta);
struct wii_abi_matrix wii_abi_matrix_negate(struct wii_abi_matrix m);
struct wii_abi_byte3 wii_abi_byte3_rotate(struct wii_abi_byte3 b);

/* 10 integer, 10 floating and 3 64-bit arguments plus a struct: exhausts
   r3-r10/f1-f8 on PPC so later arguments use the stack. Each received value
   is echoed as raw bits. */
void wii_abi_many(uint64_t *echo,
                  int a0, double f0, long long q0, int a1, float f1, int a2, double f2, long long q1,
                  int a3, float f3, int a4, double f4, int a5, float f5, int a6, double f6,
                  int a7, float f7, int a8, double f8, int a9, float f9, long long q2,
                  struct wii_abi_point3 p);

typedef long (*wii_abi_cell_proc)(long value);           /* hs_typecasting_procedure shape */
typedef unsigned long (*wii_abi_thread_proc)(void *input); /* create_thread entry shape */
typedef float (*wii_abi_real_proc)(float a, float b);
const wii_abi_cell_proc *wii_abi_cell_procs(size_t *count);
unsigned long wii_abi_thread_entry(void *input);
float wii_abi_apply(wii_abi_real_proc proc, float a, float b);
typedef int (*wii_abi_variadic_proc)(char *buffer, size_t size, const char *format, ...);
wii_abi_variadic_proc wii_abi_variadic_pointer(void);

#endif
