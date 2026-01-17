typedef int fixedfloat_t;
// Must be double the length of fixedfloat_t.
typedef long long int fixedfloat_double_t;

/* Fixed-point format: Q10.21
   Whole numbers: 0-1024
   Bit 31 indicates sign */
#define FRAC_PLACES 21
#define FORMAT (1 << FRAC_PLACES)
#define FRAC_MASK (FORMAT - 1)
#define FRAC(a) ((a) & FRAC_MASK)

#define FROMFLOAT(a) ((fixedfloat_t)(FORMAT * (a)))
#define TOFLOAT(a) (((float) (a)) / FORMAT)
#define FROMINT(a) ((fixedfloat_t)((a) << FRAC_PLACES))
#define TOINT(a) ((int)((a) >> FRAC_PLACES))
//#define IFADD(a, b) ((a) + (b)) // Just for illustration
//#define IFSUB(a, b) ((a) - (b)) // Just for illustration
#define IFMUL(a, b) ((fixedfloat_t)((((fixedfloat_double_t)(a)) * (b)) >> FRAC_PLACES))
#define IFDIV(a, b) ((fixedfloat_t)((((fixedfloat_double_t)(a)) << FRAC_PLACES) / (b)))
#define IFMOD(a, b) ((a) % (b))
#define IFFLOORF(a) ((a) & ~FRAC_MASK)
#define IFABS(a) ((a) < 0 ? -(a) : (a))

#define PI FROMFLOAT(3.141592653589793)
#define TWOPI FROMFLOAT(2 * 3.141592653589793)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

// Order was changed and some shifting is done, to avoid overflows.
#define RADIANS(a) IFDIV(IFMUL((a), PI >> 1), FROMINT(180 >> 1))
#define DEGREES(a) IFMUL(IFDIV((a), PI >> 1), FROMINT(180 >> 1))