#define WIDE(s) L ## s

unsigned int first(void)
{
	return WIDE("wide")[0];
}
