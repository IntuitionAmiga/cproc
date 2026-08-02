static long same_name(long value)
{
	return value + 1;
}

static const char same_string[] = "unit";
static const double same_float = 1.25;

extern long cross_b(long);
extern long asm_calls_c(long);

long
c_from_assembly(long value)
{
	return same_name(value) + same_string[0] + (same_float == 1.25);
}

int
main(void)
{
	return cross_b(1) == 4 && asm_calls_c(1) != 0 ? 0 : 1;
}
