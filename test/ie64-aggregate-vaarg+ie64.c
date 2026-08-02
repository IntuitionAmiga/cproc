typedef unsigned char *va_list;

struct pair {
	int first;
	long second;
};

long
next_pair(int tag, ...)
{
	va_list ap;
	struct pair p;

	__builtin_va_start(ap, tag);
	p = __builtin_va_arg(ap, struct pair);
	return p.second;
}
