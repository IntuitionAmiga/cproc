_Atomic int object;

int
compare_exchange(const int *expected)
{
	return __builtin_ie64_atomic_compare_exchange(&object, expected, 1, 5, 5);
}
