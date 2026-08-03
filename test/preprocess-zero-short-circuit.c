#if 0 && 1 / 0
#error unevaluated division selected
#endif

#if 1 || 1 % 0
int logical_short_circuit;
#endif

#if 1 ? 1 : 1 / 0
int conditional_right_short_circuit;
#endif

#if 0 ? 1 % 0 : 1
int conditional_left_short_circuit;
#endif

#if 0 && ((-9223372036854775807LL - 1) / -1)
#error unevaluated signed division overflow selected
#endif

#if 1 || ((-9223372036854775807LL - 1) % -1)
int signed_overflow_short_circuit;
#endif
