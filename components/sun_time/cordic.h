/* Define if a fixed value of N will be used. If so, then define KPROD_VALUE, which determines the K value for correction.
   The value should be selected from the kprod array, with the index being the value of N.
   If N is greater than KPROD_LENGTH, choose the last value. */
#define FIXED_N     1
#define KPROD_VALUE FROMFLOAT(0.60725293500924945172)

fixedfloat_t arccos_cordic(fixedfloat_t t, fixedfloat_t n);
fixedfloat_t arcsin_cordic (fixedfloat_t t, int n );
fixedfloat_t arctan_cordic (fixedfloat_t x, fixedfloat_t y, int n );
void cossin_cordic(fixedfloat_t beta, int n, fixedfloat_t *c, fixedfloat_t *s);