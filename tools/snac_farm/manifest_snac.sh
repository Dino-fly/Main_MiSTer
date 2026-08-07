#!/bin/bash
# Assemble the campaign manifest from what the farm actually produced.
#
# Everything here is read back from the artifact or from the container's own
# output, never from what the driver intended to build: size and md5 come from
# the file on disk, ALM utilisation and register count come from the
# fit.summary lines the container echoed into the log, and the build time comes
# from the RESULT line. A core with no file on disk gets no row and is listed
# as a failure instead.
BASE=${BASE:-/Users/mister/workspace/snac-refresh}
OUT=$BASE/cores
LOGS=$BASE/logs-win
MAN=$BASE/builds-snac.tsv

cd "$BASE" || exit 1
printf '%-26s %-32s %-9s %-9s %-34s %-30s %-22s %s\n' \
	CORE OUT_RBF DATECODE SIZE MD5 ALMS REGISTERS TIME

awk -F'\t' 'NR>1 && $5=="OK"{print $1"\t"$7}' "$MAN" | sort -u | while IFS=$'\t' read -r core rbf; do
	f=$OUT/$rbf
	[ -f "$f" ] || { printf '%-26s %-32s MISSING ARTIFACT\n' "$core" "$rbf"; continue; }
	dc=$(printf '%s' "$rbf" | sed -E 's/.*_([0-9]{8})\.rbf$/\1/')
	sz=$(wc -c < "$f" | tr -d ' ')
	md5=$(md5 -q "$f")
	alm=$(grep -hE "Logic utilization" "$LOGS/$core.log" 2>/dev/null | head -1 |
		sed -E 's/.*:[[:space:]]*//; s/[[:space:]]+/ /g')
	reg=$(grep -hE "Total registers" "$LOGS/$core.log" 2>/dev/null | head -1 |
		sed -E 's/.*:[[:space:]]*//; s/[[:space:]]+/ /g')
	t=$(grep -hE "RESULT $core OK" "$LOGS/$core.log" 2>/dev/null | head -1 |
		sed -E 's/.* OK ([0-9]+m) .*/\1/')
	printf '%-26s %-32s %-9s %-9s %-34s %-30s %-22s %s\n' \
		"$core" "$rbf" "$dc" "$sz" "$md5" "${alm:--}" "${reg:--}" "${t:--}"
done
