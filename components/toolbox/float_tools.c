#include "float_tools.h"

#include <math.h>
#include <float.h>

#ifndef fmaxf
extern float fmaxf(float, float);
#endif

bool float_is_equal(float a, float b) {
    return fabsf(a - b) <= FLT_EPSILON * fmaxf(fabsf(a), fabsf(b));
}
