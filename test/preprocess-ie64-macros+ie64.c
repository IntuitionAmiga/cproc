#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error incorrect integer byte order
#endif
#if __FLOAT_WORD_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error incorrect floating-point word order
#endif
#if __SIZEOF_POINTER__ != 8 || __SIZEOF_LONG__ != 8
#error incorrect LP64 sizes
#endif
#if __SIZEOF_LONG_DOUBLE__ != 8 || __LDBL_MANT_DIG__ != 53
#error incorrect long double format
#endif
ie64_macros_ok
#if __SIZE_MAX__ != 18446744073709551615UL
#error incorrect __SIZE_MAX__
#endif
#if __INTMAX_MAX__ != 9223372036854775807L
#error incorrect __INTMAX_MAX__
#endif
#if __SIZEOF_WCHAR_T__ != 4
#error incorrect __SIZEOF_WCHAR_T__
#endif
#if __WCHAR_MAX__ != 4294967295U
#error incorrect __WCHAR_MAX__
#endif
