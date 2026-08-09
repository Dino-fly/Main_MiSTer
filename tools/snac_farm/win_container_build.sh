#!/bin/bash
# Runs INSIDE the Quartus container on VINNIEPC. Shipped in with the source
# so nothing has to survive ssh -> cmd.exe -> bash quoting.
set -u
cd /work || exit 1
rm -rf output_files db incremental_db
# Which Quartus project to build.
#
# Not `ls | head -1`. That is what this line used to be, and it silently built the
# wrong machine for five released cores.
#
# Most repos hold one .qpf and any pick works. A handful hold several, and then two
# things go wrong at once. Sorting is locale-dependent: inside this image LC_ALL is
# en_US.UTF-8, which weighs punctuation after letters, so PSX_DualSDRAM.qpf sorts
# BEFORE PSX.qpf - and we shipped the dual-SDRAM revision of PSX, NeoGeo and Saturn.
# In those revisions sys_dual_sdram.tcl defines MISTER_DUAL_SDRAM, and every block
# patch_sys.py inserts is guarded by `ifndef MISTER_DUAL_SDRAM - so our own patch
# deletes itself and the SNAC reader is absent from a build whose log says it fitted.
# Sorting is not even the whole story: Atari5200.qpf beats Atari800.qpf and
# ColecoVision.qpf beats Ti994a.qpf under ANY locale, so we shipped an Atari 5200 as
# Atari800 and a ColecoVision as TI-99_4A. LC_ALL=C alone would not have caught those.
#
# So: never a dual-SDRAM revision - those boards have no user port at all, which is
# exactly why the patch excludes itself, and the SuperStation One cannot use them -
# and otherwise the revision whose name matches the core we asked for. CORE is passed
# in by the caller. A repo with one candidate still just takes it, which is every
# repo but five.
norm() { printf '%s' "$1" | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9'; }

CANDS=$(ls *.qpf 2>/dev/null | grep -v Q13 | grep -vEi '(_DualSDRAM|_DualSDR|_DS)\.qpf$')
[ -z "$CANDS" ] && CANDS=$(ls *.qpf 2>/dev/null | grep -v Q13)

QPF=""
if [ -n "${CORE:-}" ]; then
	want=$(norm "$CORE")
	for c in $CANDS; do
		[ "$(norm "${c%.qpf}")" = "$want" ] && { QPF=$c; break; }
	done
fi

# No name match: only safe when there is nothing to choose between.
if [ -z "$QPF" ]; then
	n=$(printf '%s\n' $CANDS | grep -c .)
	if [ "$n" -gt 1 ]; then
		echo "AMBIGUOUS_QPF core=${CORE:-?} candidates=$(printf '%s ' $CANDS)"
		exit 4
	fi
	QPF=$(printf '%s\n' $CANDS | head -1)
fi
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
