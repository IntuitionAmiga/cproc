#include <stdatomic.h>

atomic_int value;

int invalid_load_order(void)
{
	return atomic_load_explicit(&value, memory_order_release);
}
