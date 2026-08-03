struct S { int value; };

void
assign_shadow(struct S *outer)
{
	struct S { int value; } inner;
	outer = &inner;
}
