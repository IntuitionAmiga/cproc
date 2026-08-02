#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
driver=$root/ie64-cproc
stage2=$root/stage2/ie64-cproc
smoke=$root/../IntuitionEngine/sdk/tests/ie64-cproc/smoke.c
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ie64 driver test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail()
{
	printf 'test-ie64-driver: %s\n' "$*" >&2
	exit 1
}

expect_failure()
{
	local pattern=$1
	shift
	if "$@" >"$tmp/stdout" 2>"$tmp/stderr"; then
		fail "command unexpectedly succeeded: $*"
	fi
	grep -F -- "$pattern" "$tmp/stderr" >/dev/null || {
		cat "$tmp/stderr" >&2
		fail "missing diagnostic: $pattern"
	}
}

make_layout()
{
	local name=$1
	local layout=$tmp/layout-$name
	mkdir -p "$layout/cproc" "$layout/qbe" "$layout/IntuitionEngine"
	cp -- "$driver" "$root/cproc" "$root/cproc-qbe" "$layout/cproc/"
	cp -- "$root/../qbe/qbe" "$layout/qbe/qbe"
	cp -R -- "$root/../IntuitionEngine/sdk" "$layout/IntuitionEngine/sdk"
	printf '%s\n' "$layout"
}

make_complete_layout()
{
	local name=$1
	local layout
	layout=$(make_layout "$name")
	"$layout/cproc/cproc" -E -I "$layout/IntuitionEngine/sdk/include" \
		"$layout/IntuitionEngine/sdk/lib/ie64-cproc/libie64c.c" |
		"$layout/cproc/cproc-qbe" -t ie64 -fie64-unit-prefix=runtime |
		"$layout/qbe/qbe" -t ie64 -fie64-unit-prefix=runtime \
		-o "$layout/IntuitionEngine/sdk/lib/ie64-cproc/libie64c.s"
	printf '%s\n' "$layout"
}

[[ -x $driver ]] || fail "missing ie64-cproc"
[[ -x $stage2 ]] || fail "missing stage2/ie64-cproc"
[[ -f $smoke ]] || fail "missing smoke fixture"

mkdir -p "$tmp/no-sdk-root"
cp -- "$driver" "$tmp/no-sdk-root/ie64-cproc"
expect_failure 'cannot find sibling IntuitionEngine/sdk from executable path' \
	"$tmp/no-sdk-root/ie64-cproc" -E "$smoke"

mkdir -p "$tmp/a" "$tmp/b"
cp -- "$smoke" "$tmp/a/one source.c"
cp -- "$smoke" "$tmp/b/two source.c"
mkdir -p "$tmp/custom include"
cat >"$tmp/custom include/value.h" <<'EOF'
#define CUSTOM_VALUE 37
EOF
cat >"$tmp/include-test.c" <<'EOF'
#include <value.h>
int include_value(void) { return CUSTOM_VALUE; }
EOF

complete=$(make_complete_layout complete)
linkdriver=$complete/cproc/ie64-cproc

(cd /tmp && "$driver" --version) | grep -F 'ie64-cproc 1' >/dev/null
ln -s -- "$driver" "$tmp/ie64-cproc-link"
(cd /tmp && "$tmp/ie64-cproc-link" --version) | grep -F 'ie64-cproc 1' >/dev/null
(cd /tmp && "$driver" -E -o "$tmp/preprocessed file.i" "$tmp/a/one source.c")
grep -F 'main' "$tmp/preprocessed file.i" >/dev/null
(cd /tmp && "$driver" -E -I "$tmp/custom include" \
	-o "$tmp/include-test.i" "$tmp/include-test.c")
grep -F 'return 37' "$tmp/include-test.i" >/dev/null

(cd /tmp && "$driver" -S -D 'DRIVER_VALUE=1 + 2' -o "$tmp/output file.s" \
	"$tmp/a/one source.c")
[[ -s $tmp/output\ file.s ]] || fail "-S output is empty"

(cd /tmp && "$driver" -c -o "$tmp/unit file.s" "$tmp/a/one source.c")
[[ -s $tmp/unit\ file.s ]] || fail "-c output is empty"

cp -- "$smoke" "$tmp/overwrite.c"
overwrite_hash=$(sha256sum "$tmp/overwrite.c")
expect_failure 'output path resolves to input' "$driver" -S \
	-o "$tmp/overwrite.c" "$tmp/overwrite.c"
[[ $(sha256sum "$tmp/overwrite.c") == "$overwrite_hash" ]] ||
	fail "-S overwrite rejection modified its input"
ln -s -- "$tmp/overwrite.c" "$tmp/overwrite-link.c"
expect_failure 'output path resolves to input' "$driver" -c \
	-o "$tmp/overwrite-link.c" "$tmp/overwrite.c"
[[ $(sha256sum "$tmp/overwrite.c") == "$overwrite_hash" ]] ||
	fail "-c overwrite rejection modified its input"

cp -- "$smoke" "$tmp/link-overwrite.c"
link_overwrite_hash=$(sha256sum "$tmp/link-overwrite.c")
expect_failure 'output path resolves to input' "$linkdriver" \
	-o "$tmp/link-overwrite.c" "$tmp/link-overwrite.c"
[[ $(sha256sum "$tmp/link-overwrite.c") == "$link_overwrite_hash" ]] ||
	fail "link overwrite rejection modified its C input"
cp -- "$root/tools/ie64-driver-standalone.s" "$tmp/link-overwrite.s"
link_overwrite_hash=$(sha256sum "$tmp/link-overwrite.s")
ln -s -- "$tmp/link-overwrite.s" "$tmp/link-overwrite-link.s"
expect_failure 'output path resolves to input' "$driver" -nostdlib \
	-o "$tmp/link-overwrite-link.s" "$tmp/link-overwrite.s"
[[ $(sha256sum "$tmp/link-overwrite.s") == "$link_overwrite_hash" ]] ||
	fail "link overwrite rejection modified its assembly input"

cat >"$tmp/variant.c" <<'EOF'
static int private_value = DRIVER_VALUE;
#ifdef FIRST_VARIANT
int first_variant(void) { return private_value; }
#else
int second_variant(void) { return private_value; }
#endif
EOF
(cd /tmp && "$driver" -S -D DRIVER_VALUE=1 -D FIRST_VARIANT \
	-o "$tmp/variant-a.s" "$tmp/variant.c")
(cd /tmp && "$driver" -S -D DRIVER_VALUE=2 \
	-o "$tmp/variant-b.s" "$tmp/variant.c")
(cd /tmp && "$driver" -nostdlib -o "$tmp/variants.ie64" \
	"$tmp/variant-a.s" "$tmp/variant-b.s")
[[ -s $tmp/variants.ie64 ]] || fail "variant link output is empty"

(cd /tmp && "$driver" -S "$tmp/a/one source.c" "$tmp/b/two source.c")
[[ -s $tmp/a/one\ source.s && -s $tmp/b/two\ source.s ]] ||
	fail "multi-input -S outputs are missing"
rm -f -- "$tmp/a/one source.s" "$tmp/b/two source.s"
(cd /tmp && "$driver" -c "$tmp/a/one source.c" "$tmp/b/two source.c")
[[ -s $tmp/a/one\ source.s && -s $tmp/b/two\ source.s ]] ||
	fail "multi-input -c outputs are missing"

(cd /tmp && "$linkdriver" -o "$tmp/linked image.ie64" "$tmp/a/one source.c" \
	"$root/tools/ie64-driver-unit.s" "$root/tools/ie64-driver-include.s")
[[ -s $tmp/linked\ image.ie64 ]] || fail "normal link output is empty"

printf 'normal_separator:\nnop' >"$tmp/no-final-newline.s"
(cd /tmp && "$linkdriver" -o "$tmp/separated normal.ie64" \
	"$tmp/a/one source.c" "$tmp/no-final-newline.s")
[[ -s $tmp/separated\ normal.ie64 ]] ||
	fail "normal link did not separate assembly units"

(cd /tmp && "$linkdriver" -o "$tmp/cross unit.ie64" \
	"$root/tools/ie64-driver-cross-a.c" \
	"$root/tools/ie64-driver-cross-b.c" \
	"$root/tools/ie64-driver-cross.s")
[[ -s $tmp/cross\ unit.ie64 ]] || fail "cross-unit link output is empty"

(cd /tmp && "$driver" -c -o "$tmp/cross-a.s" \
	"$root/tools/ie64-driver-cross-a.c")
(cd /tmp && "$driver" -c -o "$tmp/cross-b.s" \
	"$root/tools/ie64-driver-cross-b.c")
(cd /tmp && "$linkdriver" -o "$tmp/precompiled cross.ie64" \
	"$tmp/cross-a.s" "$tmp/cross-b.s" "$root/tools/ie64-driver-cross.s")
[[ -s $tmp/precompiled\ cross.ie64 ]] || fail "precompiled cross-unit link output is empty"

(cd /tmp && "$driver" -nostdlib -o "$tmp/standalone image.ie64" \
	"$root/tools/ie64-driver-standalone.s")
[[ -s $tmp/standalone\ image.ie64 ]] || fail "-nostdlib output is empty"

printf 'org 0x1000\nhalt' >"$tmp/no-final-newline-first.s"
printf 'separator_tail:\nhalt\n' >"$tmp/no-final-newline-second.s"
(cd /tmp && "$driver" -nostdlib -o "$tmp/separated nostdlib.ie64" \
	"$tmp/no-final-newline-first.s" "$tmp/no-final-newline-second.s")
[[ -s $tmp/separated\ nostdlib.ie64 ]] ||
	fail "-nostdlib link did not separate assembly units"

(cd /tmp && "$stage2" -S -o "$tmp/stage2 output.s" "$tmp/a/one source.c")
[[ -s $tmp/stage2\ output.s ]] || fail "stage2 output is empty"

expect_failure '-o with multiple C inputs is invalid for -S' "$driver" -S \
	-o "$tmp/invalid.s" "$tmp/a/one source.c" "$tmp/b/two source.c"
expect_failure '-o with multiple C inputs is invalid for -c' "$driver" -c \
	-o "$tmp/invalid.s" "$tmp/a/one source.c" "$tmp/b/two source.c"
expect_failure '-S accepts only C inputs' "$driver" -S \
	"$root/tools/ie64-driver-unit.s"
expect_failure '-E accepts only C inputs' "$driver" -E \
	"$root/tools/ie64-driver-unit.s"
expect_failure 'label "main" redefined' "$linkdriver" -o "$tmp/duplicate.ie64" \
	"$tmp/a/one source.c" "$root/tools/ie64-driver-duplicate.s"
expect_failure 'label "memcpy" redefined' "$linkdriver" -o "$tmp/reserved.ie64" \
	"$tmp/a/one source.c" "$root/tools/ie64-driver-reserved.s"
expect_failure 'missing main in ordinary link' "$linkdriver" \
	-o "$tmp/no-main.ie64" "$root/tools/ie64-driver-unit.s"
expect_failure 'undefined label or symbol' "$driver" -nostdlib -o "$tmp/unresolved.ie64" \
	"$root/tools/ie64-driver-unresolved.s"
expect_failure 'overlapping emit' "$linkdriver" -o "$tmp/invalid-org.ie64" \
	"$tmp/a/one source.c" "$root/tools/ie64-driver-invalid-org.s"
expect_failure 'linked image overlaps reserved stack range' "$driver" -nostdlib \
	-o "$tmp/overlap.ie64" "$root/tools/ie64-driver-stack-overlap.s"

layout=$(make_layout no-include)
rm -rf -- "$layout/IntuitionEngine/sdk/include"
expect_failure 'missing SDK include directory' "$layout/cproc/ie64-cproc" -E \
	"$tmp/a/one source.c"
expect_failure 'missing SDK include directory' "$layout/cproc/ie64-cproc" -S \
	"$tmp/a/one source.c"
expect_failure 'missing SDK include directory' "$layout/cproc/ie64-cproc" -c \
	"$tmp/a/one source.c"
expect_failure 'missing SDK include directory' "$layout/cproc/ie64-cproc" -nostdlib \
	-o "$tmp/no-include-standalone.ie64" "$root/tools/ie64-driver-standalone.s"
expect_failure 'missing SDK include directory' "$layout/cproc/ie64-cproc" \
	-o "$tmp/no-include-normal.ie64" "$tmp/a/one source.c"

layout=$(make_layout no-assembler-preprocess)
rm -f -- "$layout/IntuitionEngine/sdk/bin/ie64asm"
(cd /tmp && "$layout/cproc/ie64-cproc" -E -o "$tmp/no-assembler.i" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -S -o "$tmp/no-assembler.s" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -c -o "$tmp/no-assembler-c.s" \
	"$tmp/a/one source.c")

layout=$(make_layout no-assembler-link)
rm -f -- "$layout/IntuitionEngine/sdk/bin/ie64asm"
expect_failure 'missing SDK assembler' "$layout/cproc/ie64-cproc" -nostdlib \
	-o "$tmp/no-assembler.ie64" "$root/tools/ie64-driver-standalone.s"
expect_failure 'missing SDK assembler' "$layout/cproc/ie64-cproc" \
	-o "$tmp/no-assembler-normal.ie64" "$tmp/a/one source.c"

layout=$(make_layout no-runtime-nostdlib)
rm -f -- "$layout/IntuitionEngine/sdk/lib/ie64-cproc/crt0.s" \
	"$layout/IntuitionEngine/sdk/lib/ie64-cproc/libie64c.c" \
	"$layout/IntuitionEngine/sdk/lib/ie64-cproc/libie64c.s"
(cd /tmp && "$layout/cproc/ie64-cproc" -nostdlib \
	-o "$tmp/no-runtime.ie64" "$root/tools/ie64-driver-standalone.s")

layout=$(make_complete_layout no-crt)
rm -f -- "$layout/IntuitionEngine/sdk/lib/ie64-cproc/crt0.s"
(cd /tmp && "$layout/cproc/ie64-cproc" -E -o "$tmp/no-crt.i" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -S -o "$tmp/no-crt.s" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -c -o "$tmp/no-crt-c.s" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -nostdlib -o "$tmp/no-crt.ie64" \
	"$root/tools/ie64-driver-standalone.s")
expect_failure 'missing SDK crt0.s' "$layout/cproc/ie64-cproc" \
	-o "$tmp/no-crt.ie64" "$tmp/a/one source.c"

layout=$(make_complete_layout no-library)
rm -f -- "$layout/IntuitionEngine/sdk/lib/ie64-cproc/libie64c.s"
(cd /tmp && "$layout/cproc/ie64-cproc" -E -o "$tmp/no-library.i" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -S -o "$tmp/no-library.s" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -c -o "$tmp/no-library-c.s" \
	"$tmp/a/one source.c")
(cd /tmp && "$layout/cproc/ie64-cproc" -nostdlib \
	-o "$tmp/no-library.ie64" "$root/tools/ie64-driver-standalone.s")
expect_failure 'missing SDK libie64c.s' "$layout/cproc/ie64-cproc" \
	-o "$tmp/no-library.ie64" "$root/tools/ie64-driver-invalid.c"

printf '%s\n' 'test-ie64-driver: all checks passed'
