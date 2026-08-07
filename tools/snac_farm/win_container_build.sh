#!/bin/bash
# Runs INSIDE the Quartus container on VINNIEPC. Shipped in with the source
# so nothing has to survive ssh -> cmd.exe -> bash quoting.
set -u
cd /work || exit 1
rm -rf output_files db incremental_db
QPF=$(ls *.qpf 2>/dev/null | grep -v Q13 | head -1)
[ -z "$QPF" ] && { echo NO_QPF; exit 4; }
REV=${QPF%.qpf}
[ -f "$REV.qsf" ] || { echo NO_QSF; exit 4; }
printf '\nset_global_assignment -name NUM_PARALLEL_PROCESSORS %s\n' "${NPROC:-4}" >> "$REV.qsf"
quartus_sh --flow compile "$REV" > /tmp/b.log 2>&1
RBF=$(ls output_files/*.rbf 2>/dev/null | head -1)
if [ -n "$RBF" ]; then
	cp "$RBF" /work/BUILT.rbf
	grep -hE "Logic utilization|Total registers" output_files/*.fit.summary 2>/dev/null | head -2
	echo RBF_OK
else
	echo NO_RBF
	grep -iE "^Error" /tmp/b.log | head -6
	exit 5
fi
