static long same_name(long value)
{
	return value + 2;
}

static const char same_string[] = "unit";
static const double same_float = 1.25;

extern long asm_from_c(long);

long
cross_b(long value)
{
	return asm_from_c(same_name(value)) + same_string[0] - 'u'
		+ (same_float == 1.25) - 1;
}
