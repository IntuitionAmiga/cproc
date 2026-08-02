signed char
rsb(signed char x)
{
	return x;
}

unsigned char
rub(unsigned char x)
{
	return x;
}

short
rsh(short x)
{
	return x;
}

unsigned short
ruh(unsigned short x)
{
	return x;
}

int
rsw(int x)
{
	return x;
}

unsigned int
ruw(unsigned int x)
{
	return x;
}

extern long narrow(signed char, unsigned char, short, unsigned short, int,
	unsigned int, signed char, unsigned char, short, unsigned short, int,
	unsigned int, _Bool);

long
call_narrow(void)
{
	return narrow(-1, 255, -2, 65535, -3, 0xffffffffu,
		-4, 254, -5, 65534, -6, 0xfffffffeu, 7);
}
