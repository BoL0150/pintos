#include <stdint.h>
#ifndef FIXED_POINT_H
#define FIXED_POINT_H

typedef int32_t fp32_t;

static int32_t frac = 16384;

static inline fp32_t to_fp(int32_t n) {
    return n * frac;
}
static inline int32_t to_int_rounding_to_zero(fp32_t fp) {
    return fp / frac;
}
static inline int32_t to_int_rounding_to_nearest(fp32_t fp) {
    if (fp >= 0) {
        return (fp + frac / 2) / frac;
    } else {
        return (fp - frac / 2) / frac;
    }
}
static inline fp32_t fp_add(fp32_t fp1, fp32_t fp2) {
    return fp1 + fp2;
}
static inline fp32_t fp_subtract(fp32_t fp1, fp32_t fp2) {
    return fp1 - fp2;
}
static inline fp32_t fp_add_int(fp32_t fp, int32_t n) {
    return fp + n * frac;
}
static inline fp32_t fp_subtract_int(fp32_t fp, int32_t n) {
    return fp - n * frac;
}
static inline fp32_t fp_mul(fp32_t fp1, fp32_t fp2) {
    return ((int64_t) fp1) * fp2 / frac;
}
static inline fp32_t fp_mul_int(fp32_t fp, int32_t n) {
    return fp * n;
}
static inline fp32_t fp_divide(fp32_t fp1, fp32_t fp2) {
    return ((int64_t) fp1) * frac / fp2;
}
static inline fp32_t fp_divide_int(fp32_t fp, int32_t n) {
    return fp / n;
}

#endif