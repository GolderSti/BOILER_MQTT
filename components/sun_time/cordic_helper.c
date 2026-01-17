#include "fixedpoint.h"
#include "cordic.h"
#include "cordic_helper.h"

fixedfloat_t ifsin(fixedfloat_t a) {
    fixedfloat_t cosVal;
    fixedfloat_t sinVal;
    sincos(a, &sinVal, &cosVal);
    return sinVal;
}

fixedfloat_t ifcos(fixedfloat_t a) {
    fixedfloat_t cosVal;
    fixedfloat_t sinVal;
    sincos(a, &sinVal, &cosVal);
    return cosVal;
}

void ifsincos(fixedfloat_t a, fixedfloat_t *sinout, fixedfloat_t *cosout) {
    cossin_cordic(a, ITERATIONS, cosout, sinout);
}

fixedfloat_t iftan(fixedfloat_t a) {
    fixedfloat_t cosVal;
    fixedfloat_t sinVal;
    sincos(a, &sinVal, &cosVal);
    return IFDIV(sinVal, cosVal);
}

fixedfloat_t ifacos(fixedfloat_t a) {
    return arccos_cordic(a, ITERATIONS);
}

fixedfloat_t ifasin(fixedfloat_t a) {
    return arcsin_cordic(a, ITERATIONS);
}

fixedfloat_t ifatan2(fixedfloat_t x, fixedfloat_t y) {
    return arctan_cordic(x, y, ITERATIONS);
}