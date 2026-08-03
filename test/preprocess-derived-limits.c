#define INT_MAX_VALUE 2147483647
#define UINT_MAX_VALUE (INT_MAX_VALUE * 2U + 1)
#define LONG_MAX_VALUE 9223372036854775807L
#define ULONG_MAX_VALUE (LONG_MAX_VALUE * 2UL + 1)

#if UINT_MAX_VALUE < ULONG_MAX_VALUE
int derived_limits_compare;
#else
#error derived limit comparison failed
#endif
