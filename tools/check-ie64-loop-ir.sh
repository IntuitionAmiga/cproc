#!/bin/sh
set -eu

ccqbe=${1:-./cproc-qbe}
il=$("$ccqbe" -t ie64 - <<'C'
int widen(const short *p) { return *p; }
long wide(const short *p) { return *p; }
int narrow(int x) { return (short)x; }
void dot(int *acc, const short *p, unsigned n) {
    *acc = 0;
    for (unsigned i = 0; i < n; ++i)
        *acc += p[i];
}
union bits { int w; short h[2]; };
int member(volatile short *p, union bits *u) {
    *p = 1;
    u->w = 42;
    return u->h[0] + *p;
}
C
)
body() {
    printf "%s\n" "$il" | sed -n "/function .*\\\$$1(/,/^}/p"
}
widen=$(body widen)
wide=$(body wide)
narrow=$(body narrow)
dot=$(body dot)
member=$(body member)
test -n "$widen" && test -n "$wide" && test -n "$narrow" && test -n "$dot" && test -n "$member"
if printf "%s\n" "$widen" | rg -q "extsh"; then exit 1; fi
printf "%s\n" "$wide" | rg -q "=l extsh"
printf "%s\n" "$narrow" | rg -q "extsh"
printf "%s\n" "$dot" | rg -q "loadsh.* #!1"
test "$(printf "%s\n" "$dot" | rg -c "storew.* #!2")" -eq 2
printf "%s\n" "$dot" | rg -q "loadw.* #!2"
if printf "%s\n" "$member" | rg -q " #![12]"; then exit 1; fi
