/* Resistance 3: native replacement of the ADS depth-of-field job kernels
 * (igjob 419). See r3_native_dof.c. */
#pragma once
#include <stdint.h>

#ifndef R3_NATIVE_DOF_DEFAULT
#define R3_NATIVE_DOF_DEFAULT 1
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;
/* Runs the kernel at (owner image, offset) natively on ctx's LS/registers.
 * Returns 1 if handled (state equals what the lifted kernel leaves at its
 * return), 0 to fall back to the lifted kernel. */
int r3_native_dof_kernel(struct spu_context* ctx, uint32_t owner_img, uint32_t off);
#ifdef __cplusplus
}
#endif
