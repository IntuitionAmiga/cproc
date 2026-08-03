#define UINT_MAX 4294967295U
#define ULONG_MAX 18446744073709551615UL
#if !(UINT_MAX < ULONG_MAX)
#error unsigned comparison failed
#endif
#if -1 >= 0
#error signed negative comparison failed
#endif
#if !(-1 < 0)
#error signed comparison failed
#endif
#if (-1 < UINT_MAX)
#error usual unsigned conversion failed
#endif
unsigned_ok
