typedef int array4[4];

void discard(const array4 *source, int (*destination)[4])
{
	destination = source;
}
