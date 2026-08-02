#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=$root/../IntuitionEngine/sdk
source=$sdk/lib/ie64-cproc/libie64c.c
qbe=$root/../qbe/qbe
output=${1:-$sdk/lib/ie64-cproc/libie64c.s}

fail()
{
	printf 'generate-ie64-runtime: %s\n' "$*" >&2
	exit 1
}

[[ -f $source ]] || fail "missing $source"
[[ -x $root/cproc && -x $root/cproc-qbe ]] || fail "build cproc first"
[[ -x $qbe ]] || fail "missing QBE executable $qbe"

"$root/cproc" -E -I "$sdk/include" "$source" |
	"$root/cproc-qbe" -t ie64 -fie64-unit-prefix=runtime |
	"$qbe" -t ie64 -fie64-unit-prefix=runtime -o "$output"
