volatile unsigned long x;

unsigned long
f(void)
{
	unsigned long a = x;
	unsigned long b = x;
	x = a + b;
	x = 3;
	return b;
}

void
discard(void)
{
	(void)x;
	(void)x;
}
