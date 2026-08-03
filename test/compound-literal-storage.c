int *f(void) {
	return &(static int){4};
}
int g(void) {
	return (constexpr int){9};
}
