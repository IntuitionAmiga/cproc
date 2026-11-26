int f(unsigned int x)
{
	switch (x) {
	case -1: return 1;
	case 4294967295U: return 2;
	}
	return 0;
}
