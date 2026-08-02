_Static_assert((char)-1 < 0, "signed char");
_Static_assert(sizeof(long) == 8, "LP64 long");
_Static_assert(sizeof(void *) == 8, "LP64 pointer");
_Static_assert(sizeof(L'a') == 4, "wide character");
_Static_assert(sizeof(_Bool) == 1 && _Alignof(_Bool) == 1, "bool layout");
_Static_assert(sizeof(short) == 2 && _Alignof(short) == 2, "short layout");
_Static_assert(sizeof(int) == 4 && _Alignof(int) == 4, "int layout");
_Static_assert(_Alignof(long) == 8 && _Alignof(void *) == 8, "LP64 alignment");
_Static_assert(sizeof(float) == 4 && _Alignof(float) == 4, "float layout");
_Static_assert(sizeof(double) == 8 && _Alignof(double) == 8, "double layout");

enum ie64_enum { IE64_ZERO, IE64_ONE };
_Static_assert(sizeof(enum ie64_enum) == sizeof(int), "enum representation");

struct ie64_layout {
	char byte;
	long word;
	short half;
};
_Static_assert(_Alignof(struct ie64_layout) == 8, "aggregate alignment");
_Static_assert(sizeof(struct ie64_layout) == 24, "aggregate size");
