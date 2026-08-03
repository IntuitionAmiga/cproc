#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
matrix=$root/test/c23-matrix.tsv

awk -F '\t' '
/^#/ || NF == 0 { next }
NF != 4 { print "invalid matrix row: " $0 > "/dev/stderr"; bad=1; next }
$2 != "mandatory" && $2 != "optional" && $2 != "inapplicable" { print "invalid classification: " $0 > "/dev/stderr"; bad=1 }
$3 != "pass" && $3 != "open" && $3 != "inapplicable" { print "invalid status: " $0 > "/dev/stderr"; bad=1 }
$3 == "pass" && $4 == "-" { print "passing row has no test: " $0 > "/dev/stderr"; bad=1 }
END { exit bad }
' "$matrix"

while IFS='\t' read -r feature class status testpath
do
	case $feature in \#*|'') continue;; esac
	if [ "$status" = pass ] && [ ! -f "$root/$testpath" ]; then
		echo "C23 matrix test is missing: $testpath" >&2
		exit 1
	fi
done < "$matrix"

if awk -F '\t' '$2 == "mandatory" && $3 == "open" { found=1 } END { exit !found }' "$matrix"; then
	if grep -q '__STDC_VERSION__=202311L' "$root/main.c"; then
		echo '__STDC_VERSION__ advertises C23 while mandatory matrix rows remain open' >&2
		exit 1
	fi
else
	grep -q '__STDC_VERSION__=202311L' "$root/main.c" || {
		echo '__STDC_VERSION__ must advertise C23 after all mandatory rows pass' >&2
		exit 1
	}
fi

echo 'C23 feature matrix is consistent'
