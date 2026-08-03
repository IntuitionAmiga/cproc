typedef int array4[4];

int array_qualifiers(const array4 *left, const int (*right)[4])
{
	right = left;
	return 1;
}
