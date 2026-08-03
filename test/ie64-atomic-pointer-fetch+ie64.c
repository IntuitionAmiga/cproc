_Atomic(int *) pointer;

int *fetch_add_pointer(void)
{
	return __builtin_ie64_atomic_fetch_add(&pointer, 3, 5);
}

int *fetch_sub_pointer(void)
{
	return __builtin_ie64_atomic_fetch_sub(&pointer, 2, 5);
}
