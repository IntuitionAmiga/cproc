#define CAT(a,b) a ## b
#define CAT3(a,b,c) a ## b ## c
#define X 12
CAT(hello,_world)
CAT3(one,_,two)
CAT(X,34)
CAT(L,"wide")
#define W 42
CAT(W,items[1])
CAT(words tail,_end)
CAT(,x)
CAT(x,)
CAT(,)
CAT3(a,,b)
