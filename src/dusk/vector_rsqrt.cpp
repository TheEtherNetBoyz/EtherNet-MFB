#include "dusk/vector_rsqrt.h"

#include <dolphin/ppc_math.h>

#include <math.h>

namespace {
int s_useNativeVectorRsqrt = 0;
}

extern "C" void dusk_set_native_vector_rsqrt(int enabled) {
    s_useNativeVectorRsqrt = enabled != 0;
}

extern "C" int dusk_get_native_vector_rsqrt(void) {
    return s_useNativeVectorRsqrt;
}

extern "C" float dusk_vector_rsqrt(float x) {
    return s_useNativeVectorRsqrt ? 1.0f / sqrtf(x) : ppc_rsqrte(x);
}

extern "C" float dusk_fast_sqrt(float x) {
    return s_useNativeVectorRsqrt ? sqrtf(x) : (float)(frsqrte((double)x) * x);
}
