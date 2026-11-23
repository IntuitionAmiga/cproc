#include "preprocess-line-macro.h"
here HERE
again HERE
where WHERE
ASSERT(1 == 2);
ASSERT(
	3 == 4);
twice TWICE(__LINE__)
nest NEST(HERE __LINE__)
span TWICE(
__LINE__
) after __LINE__
#line 500 "relocated.c"
HERE WHERE
ASSERT(0);
