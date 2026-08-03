struct item;
struct item;
struct item { int value; };
enum state : int;
enum state : int { stopped, running = 4 };

int compatible_tags(void)
{
	struct item { int value; } local;
	enum state { stopped, running = 4 } state;
	return __builtin_types_compatible_p(typeof(local), struct item)
	    && __builtin_types_compatible_p(typeof(state), enum state);
}
