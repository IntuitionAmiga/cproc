[[deprecated("old")]] extern int old_value;
[[maybe_unused]] static int helper;
[[nodiscard("check")]] int result(void) { return 2; }
[[noreturn]] void stop(void);
[[reproducible]] int pure_value(int value);
[[unsequenced]] int square(int value);
