/* CORDIC Helper functions. Presently developed around the CORDIC implementation from https://people.sc.fsu.edu/~jburkardt/cpp_src/cordic/cordic.html */
#define ITERATIONS 20

fixedfloat_t ifsin(fixedfloat_t a);
fixedfloat_t ifcos(fixedfloat_t a);
void ifsincos(fixedfloat_t a, fixedfloat_t *sinout, fixedfloat_t *cosout);
fixedfloat_t iftan(fixedfloat_t a);
fixedfloat_t ifacos(fixedfloat_t a);
fixedfloat_t ifasin(fixedfloat_t a);
#define ifatan(a) ifatan2(FROMINT(1), a)
fixedfloat_t ifatan2(fixedfloat_t x, fixedfloat_t y);

#define sin(x) ifsin(x)
#define cos(x) ifcos(x)
#define sincos(x, y, z) ifsincos(x, y, z)
#define tan(x) iftan(x)
#define acos(x) ifacos(x)
#define asin(x) ifasin(x)
#define atan(x) ifatan(x)
#define atan2(x, y) ifatan2(x, y)