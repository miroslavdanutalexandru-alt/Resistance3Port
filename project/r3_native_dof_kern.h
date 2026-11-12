/* Resistance 3 native DOF kernels: shared definitions for the generated
 * r3_native_dof_gen_*.c files (scratch\dof_native\transpile.py) and the
 * offline verifier (scratch\dof_native\verify_dof.c).
 *
 * Each r3dof_gen_<img>() runs one call of a job-419 kernel image on a plain
 * LS buffer + register file: entry_pc / ret_lr / base are absolute LS
 * addresses. Returns 1 when the kernel returned to ret_lr, -1 when it
 * branched somewhere outside the image (*fail_pc = target; the state is the
 * exact SPU state at that branch, so the lifted code can carry on from it).
 *
 * Must be compiled with the same flags as the lifted SPU code (SPU_SOURCES:
 * AVX2/FMA, SPU_XF_NO_EXACT) and run with the SPU threads' MXCSR (FTZ|DAZ|RZ):
 * the float helpers are then the very same instructions as in the lifted
 * code, which is what makes the result bit-identical. */
#ifndef R3_NATIVE_DOF_KERN_H
#define R3_NATIVE_DOF_KERN_H
#include <stdint.h>
#include "spu_helpers.h"

static inline __attribute__((always_inline)) u128 r3dof_lsr(const uint8_t* ls, uint32_t lsa)
{
    u128 v;
    const __m128i x = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(ls + (lsa & SPU_LS_MASK & ~0xFu))),
                                       _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12));
    memcpy(&v, &x, 16);
    return v;
}
static inline __attribute__((always_inline)) void r3dof_lsw(uint8_t* ls, uint32_t lsa, u128 val)
{
    __m128i x; memcpy(&x, &val, 16);
    _mm_storeu_si128((__m128i*)(ls + (lsa & SPU_LS_MASK & ~0xFu)),
                     _mm_shuffle_epi8(x, _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12)));
}
#define LSR(a)    r3dof_lsr(ls, (uint32_t)(a))
#define LSW(a, v) r3dof_lsw(ls, (uint32_t)(a), (v))

#define R3DOF_GEN_DECL(n) int r3dof_gen_##n(uint8_t* ls, u128* gpr, uint32_t _b, uint32_t entry_pc, \
                                            uint32_t ret_lr, uint32_t* fail_pc)
R3DOF_GEN_DECL(618);
R3DOF_GEN_DECL(619);
R3DOF_GEN_DECL(620);
R3DOF_GEN_DECL(621);
R3DOF_GEN_DECL(622);
R3DOF_GEN_DECL(623);
#endif
