struct small {
	int tag;
	double value;
};

struct wide {
	long first;
	char byte;
	short half;
};

extern float fp_exhaust(float, float, float, float, float, float, float,
	double, float);
extern long overflow_aggregate(long, long, long, long, long, long,
	struct small, long);

struct small
make_small(int tag, double value, long a, long b, long c, long d, long e,
	long f, long g)
{
	struct small result;

	result.tag = tag + a + b + c + d + e + f + g;
	result.value = value;
	return result;
}

long
copy_isolation(struct small value)
{
	value.tag = 99;
	return value.tag;
}

long
call_matrix(struct small *value)
{
	struct small made;
	long (*indirect)(struct small) = copy_isolation;

	made = make_small(value->tag, value->value, 1, 2, 3, 4, 5, 6, 7);
	indirect(made);
	fp_exhaust(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f,
		7.0, 8.0f);
	return overflow_aggregate(1, 2, 3, 4, 5, 6, made, 7);
}

long
vararg_matrix(int tag, double named, struct small fixed, ...)
{
	__builtin_va_list ap;
	struct small a;
	struct wide b;
	long first;
	long last;

	__builtin_va_start(ap, fixed);
	a = __builtin_va_arg(ap, struct small);
	first = __builtin_va_arg(ap, long);
	b = __builtin_va_arg(ap, struct wide);
	last = __builtin_va_arg(ap, long);
	return tag + (named == 2.0) + fixed.tag + a.tag + first + b.first + last;
}

long
call_vararg(struct small a, struct wide b)
{
	struct small fixed = {3, 4.0};

	return vararg_matrix(1, 2.0, fixed, a, 5L, b, 6L);
}
