/* unused internal-linkage functions and the data only they refer to are dropped */
int sink(int);
int (*fnptr)(void);

static int unused_plain(int x) { return x + 1; }
static inline int unused_inline(int x) { return x * 2; }
static int unused_chain_a(int);
static int unused_chain_b(int x) { return unused_chain_a(x - 1); }
static int unused_chain_a(int x) { return x ? unused_chain_b(x) : 0; }
static int unused_recursive(int x) { return x ? unused_recursive(x - 1) : 0; }
static const char *unused_name(void) { static int n = 3; return n ? "dead string" : __func__; }

static int used_direct(int x) { return x + 3; }
static int used_address(void) { return 4; }
static int used_data(void) { return 5; }
static int used_chain_leaf(int x) { return x + 6; }
static int used_chain_mid(int x) { return used_chain_leaf(x); }
static int used_forward(int);
static int used_ctor(void) { return 8; }
static int (*const table[])(void) = {used_data};

int
entry(int x)
{
	fnptr = used_address;
	return sink(used_direct(x) + used_chain_mid(x) + used_forward(x) + table[0]());
}

static int used_forward(int x) { return x - 1; }
__attribute__((constructor)) static void ctor(void) { sink(used_ctor()); }
