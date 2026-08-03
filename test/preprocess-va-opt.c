#define CALL(fmt, ...) invoke(fmt __VA_OPT__(,) __VA_ARGS__)
CALL("none")
CALL("two", 1, 2)
