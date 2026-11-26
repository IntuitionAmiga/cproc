#!/bin/sh
set -eu

cc=${1:-./cproc-qbe}
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT HUP INT TERM

"$cc" -t ie64 -o "$tmp" test/dense-switch+ie64.c.fixture
awk -F ', ' '
	/^[[:space:]]+switch / {
		tables++
		if (NF == 12 && $2 == $5) hole = 1
	}
	END { exit !(tables == 1 && hole) }
' "$tmp"
"$cc" -t x86_64-sysv -o "$tmp" test/dense-switch+ie64.c.fixture
if rg -q '^[[:space:]]+switch ' "$tmp"
then
	printf '%s\n' 'jump table emitted for the host target' >&2
	exit 1
fi
