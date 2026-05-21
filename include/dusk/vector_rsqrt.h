#ifndef DUSK_VECTOR_RSQRT_H
#define DUSK_VECTOR_RSQRT_H

#ifdef __cplusplus
extern "C" {
#endif

void dusk_set_native_vector_rsqrt(int enabled);
int dusk_get_native_vector_rsqrt(void);
float dusk_vector_rsqrt(float x);
float dusk_fast_sqrt(float x);

#ifdef __cplusplus
}
#endif

#endif // DUSK_VECTOR_RSQRT_H
