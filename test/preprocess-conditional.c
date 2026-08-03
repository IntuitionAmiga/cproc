#define A 4
#define B (A + 3)
#if defined(A) && B == 7
good_one
#else
bad_one
#endif
#if 0
#if 1
bad_nested
#endif
#if undefined_function_like_macro(argument)
bad_inactive_expression
#endif
#elif !defined(MISSING) && (8 >> 1) >= 4
good_two
#endif
#define VERSION_AT_LEAST(major, minor) ((VERSION_MAJOR << 16) + VERSION_MINOR >= ((major) << 16) + (minor))
#define VERSION_AT_LEAST_(major, minor) VERSION_AT_LEAST(major, minor)
#define VERSION_MAJOR 4
#define VERSION_MINOR 2
#if VERSION_AT_LEAST_(4, 1) && !VERSION_AT_LEAST(5, 0)
good_function_macro
#else
bad_function_macro
#endif
#define TYPE_TOKENS signed char
TYPE_TOKENS prior_expansion;
#define signed +0
#define char +0
#if TYPE_TOKENS == 0
good_later_macro_definition
#else
bad_later_macro_definition
#endif
#if (L'\0' - 1) < 0 && '\x41' == 65 && '\101' == 65
good_character_constants
#else
bad_character_constants
#endif
#if (9223372036854775807L * 2UL + 1) == 18446744073709551615UL
good_uint64_constant
#else
bad_uint64_constant
#endif
#if 1 ? 0 ? 0 : 1 : 0
good_conditional_expression
#else
bad_conditional_expression
#endif
