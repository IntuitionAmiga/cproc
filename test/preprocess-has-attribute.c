#if __has_c_attribute(deprecated) != 201904
#error deprecated date is wrong
#endif
#if __has_c_attribute(fallthrough) != 201910
#error fallthrough date is wrong
#endif
#if __has_c_attribute(maybe_unused) != 202106
#error maybe_unused date is wrong
#endif
#if __has_c_attribute(nodiscard) != 202003
#error nodiscard date is wrong
#endif
#if __has_c_attribute(noreturn) != 202202
#error noreturn date is wrong
#endif
#if __has_c_attribute(reproducible) != 202207
#error reproducible date is wrong
#endif
#if __has_c_attribute(unsequenced) != 202207
#error unsequenced date is wrong
#endif
#if __has_c_attribute(unknown_attribute)
#error unknown attribute must not be reported
#endif
attributes_ok
