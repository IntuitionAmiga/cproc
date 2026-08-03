_Atomic int value;

int
load(void)
{
	return value;
}

void
store(int v)
{
	value = v;
}
