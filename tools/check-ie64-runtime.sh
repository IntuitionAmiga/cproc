#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=$root/../IntuitionEngine/sdk
checked=$sdk/lib/ie64-cproc/libie64c.s
tmp=$(mktemp "${TMPDIR:-/tmp}/libie64c.XXXXXX.s")
trap 'rm -f "$tmp"' EXIT HUP INT TERM

fail()
{
	printf 'check-ie64-runtime: %s\n' "$*" >&2
	exit 1
}

[[ -f $checked ]] || fail "missing generated runtime $checked"
"$root/tools/generate-ie64-runtime.sh" "$tmp"

cmp "$checked" "$tmp" || fail "generated libie64c.s is stale"
printf '%s\n' 'check-ie64-runtime: generated assembly is current'
