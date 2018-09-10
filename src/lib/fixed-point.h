#ifndef FIXED_POINT
#define FIXED_POINT

/* Fixed-point real arithmetic */
#define _P 17        // Integer bits
#define _Q 14        // Fraction bits
#define _S 1         // Sign bit
#define _F 1 << (_Q) // Fraction

/* Here x and y are fixed-point (17.14) reals and n is a 32 bit integer */
#define _TO_FP(n) ((n) * (_F))
#define _TO_INT_ZERO(x) ((x) / (_F))
#define _TO_INT_NEAREST(x) (((x) >= 0 ? ((x) + (_F) / 2) / (_F) : \
                             ((x) - (_F) / 2) / (_F)))
#define _ADD(x, y) ((x) + (y))
#define _SUB(x, y) ((x) - (y))
#define _ADD_INT(x, n) ((x) + (n) * (_F))
#define _SUB_INT(x, n) ((x) - (n) * (_F))
#define _INT_SUB(n, x) ((n) * (_F) - (x))
#define _MULTIPLY(x, y) (((int64_t)(x)) * (y) / (_F))
#define _MULTIPLY_INT(x, n) ((x) * (n))
#define _DIVIDE(x, y) (((int64_t)(x)) * (_F) / (y))
#define _DIVIDE_INT(x, n) ((x) / (n))

#endif
