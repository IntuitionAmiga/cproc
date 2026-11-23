#define HERE __LINE__
#define WHERE __FILE__
#define ASSERT(e) ((e) ? 0 : fail(#e, __FILE__, __LINE__))
#define TWICE(x) x __LINE__ x
#define NEST(x) TWICE(x) HERE
