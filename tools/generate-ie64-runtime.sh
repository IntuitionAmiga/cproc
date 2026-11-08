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
[[ -x $root/cproc-qbe ]] || fail "build cproc first"
[[ -x $qbe ]] || fail "missing QBE executable $qbe"

tmp=$(mktemp "${TMPDIR:-/tmp}/generate-ie64-runtime.XXXXXX")
trap 'rm -f "$tmp"' EXIT HUP INT TERM

"$root/cproc-qbe" -t ie64 -DIE_TARGET_IE64=1 -I "$sdk/include" "$source" |
	"$qbe" -t ie64 -o "$tmp"

# The generated file is Intuition Engine output, not cproc source: it carries
# the licence of libie64c.c, not the licence of this generator.
{
	printf '%s\n' \
		'; SPDX-FileCopyrightText: 2024-2026 Zayn Otley' \
		'; SPDX-License-Identifier: LicenseRef-IntuitionEngine-Proprietary' \
		'; See LICENSES/IE-Proprietary.txt.' \
		''
	cat "$tmp"
} >"$output"
