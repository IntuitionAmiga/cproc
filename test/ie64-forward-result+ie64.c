extern unsigned short a(void);
extern unsigned short b(unsigned short);

unsigned short f(void)
{
	return b(a());
}

signed short g(void)
{
	return (signed short)a();
}
