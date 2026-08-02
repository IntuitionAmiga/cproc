struct pair {
	unsigned long first;
	unsigned long second;
};

volatile struct pair dst;
struct pair src;

void
f(void)
{
	dst = src;
}
