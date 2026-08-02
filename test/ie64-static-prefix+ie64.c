static int local(void) {
	return 7;
}

static int value;

int f(void) {
	value = local();
	return value;
}
