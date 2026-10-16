/*
 * RENDER_FIRST_PERSON_PROJECTION.C
 *
 * HWI-084: the first-person weapon's GX projection (wii_render.h,
 * wii_gx_first_person_projection). Strict C11, no libogc or engine header:
 * the GX side links it, the engine side (render_first_person_engine.c) calls
 * it, and tools/wii/test_first_person_render.py builds it on the host.
 *
 * The Xbox draws the weapon with the frame's projection whose depth terms
 * rasterizer_set_frustum_z replaced (render_camera_hack_frustum_z: x and y
 * kept, depth from the first-person near and far clip distances), and
 * writes the stencil, which every surface drawn after it is rejected by. GX
 * has no stencil: the same projection maps the weapon's depth into the front
 * slice of the range instead. GX_PERSPECTIVE takes z in [-w, 0] (w = -z_view);
 * with the first-person depth row z = a z_view + b (a = -n / (f - n),
 * b = -f n / (f - n): -w at the near plane, 0 at the far one), the slice's
 * row is z' = s z + (s - 1) w: -w at the near plane (depth 0) still, and
 * -(1 - s) w at the far one (depth s). The world, drawn with its own near
 * plane, has depth below s only within a hair of that plane, so the weapon
 * is over it, as the stencil makes it.
 */
#include <math.h>

#include "wii_render.h"

int wii_gx_first_person_projection(const float frame[4][4], float near_distance, float far_distance, float slice,
                                   float out[4][4])
{
    if (!isfinite(near_distance) || !isfinite(far_distance) || !isfinite(slice) || !(near_distance > 0.0f) ||
        !(far_distance > near_distance) || !(slice > 0.0f) || !(slice <= 1.0f))
        return 0;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            out[row][column] = frame[row][column];
    const float range = far_distance - near_distance;
    out[2][0] = 0.0f;
    out[2][1] = 0.0f;
    out[2][2] = (1.0f - slice) - slice * near_distance / range;
    out[2][3] = -slice * far_distance * near_distance / range;
    out[3][0] = 0.0f;
    out[3][1] = 0.0f;
    out[3][2] = -1.0f;
    out[3][3] = 0.0f;
    return 1;
}
