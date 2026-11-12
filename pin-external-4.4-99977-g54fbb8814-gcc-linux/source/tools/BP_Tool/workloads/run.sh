#!/usr/bin/env bash
# Run each workload under bp_tool and print its control transfer counts by category.
# With --sim, also run the given branch predictor simulator(s) and print their results.
# Usage: [PIN_ROOT=/path/to/pin-kit] ./run.sh [--sim SPEC] [workload ...]   (default: all workloads)
#   SPEC is passed to bp_tool's -sim knob: comma-separated name[:args] as registered with
#   REGISTER_BRANCH_SIMULATOR, e.g. --sim baseline or --sim baseline,other:arg
set -u

usage() {
	sed -n '2,6s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"
}

SIM=""
WORKLOADS=()
while [ $# -gt 0 ]; do
	case "$1" in
	--sim)
		if [ $# -lt 2 ] || [ -z "$2" ]; then
			echo "ERROR: --sim needs a simulator spec (e.g. --sim baseline)" >&2
			exit 1
		fi
		SIM="$2"
		shift 2
		;;
	--sim=*)
		SIM="${1#--sim=}"
		if [ -z "$SIM" ]; then
			echo "ERROR: --sim needs a simulator spec (e.g. --sim=baseline)" >&2
			exit 1
		fi
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	--)
		shift
		WORKLOADS+=("$@")
		break
		;;
	-*)
		echo "ERROR: unknown option '$1'" >&2
		usage >&2
		exit 1
		;;
	*)
		WORKLOADS+=("$1")
		shift
		;;
	esac
done
if [ ${#WORKLOADS[@]} -eq 0 ]; then
	WORKLOADS=(loop sort recursion dispatch)
fi

WL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL_DIR="$(cd "$WL_DIR/.." && pwd)"
PIN_ROOT="${PIN_ROOT:-$(cd "$TOOL_DIR/../../.." && pwd)}"
if [ ! -f "$PIN_ROOT/intel64/lib/libpin.a" ]; then
	echo "ERROR: $PIN_ROOT is missing Pin's libraries (intel64/lib/libpin.a);" \
			"set PIN_ROOT to a complete Pin kit" >&2
	exit 1
fi
PIN="$PIN_ROOT/pin"
TOOL="$TOOL_DIR/obj-intel64/bp_tool.so"
MIN_INSTS=100000
CATEGORIES=(COND JMP_DIR JMP_IND CALL_DIR CALL_IND RET)

# Stats key prefix for each simulator spec, matching SimDriver::Init in bp_sim.cpp
# (characters other than [A-Za-z0-9_-] become '_').
SIM_KEYS=()
TOOL_ARGS=()
if [ -n "$SIM" ]; then
	TOOL_ARGS=(-sim "$SIM")
	IFS=, read -ra specs <<< "$SIM"
	for spec in "${specs[@]}"; do
		[ -n "$spec" ] && SIM_KEYS+=("${spec//[^A-Za-z0-9_-]/_}")
	done
fi

# Build the pintool and the workloads.
make -s -C "$TOOL_DIR" PIN_ROOT="$PIN_ROOT" obj-intel64/bp_tool.so || {
	echo "ERROR: failed to build bp_tool" >&2
	exit 1
}
make -s -C "$WL_DIR" -- "${WORKLOADS[@]}" || {
	echo "ERROR: failed to build workloads" >&2
	exit 1
}

# Read one value from a "key = value" stats file.
stat() {
	awk -F' = ' -v k="$2" '$1 == k { print $2; exit }' "$1"
}

status=0
for wl in "${WORKLOADS[@]}"; do
	stats="$WL_DIR/$wl.stats"
	rm -f "$stats"
	echo "=== $wl ==="
	if ! "$PIN" -t "$TOOL" -o "$stats" ${TOOL_ARGS[@]+"${TOOL_ARGS[@]}"} -- "$WL_DIR/$wl"; then
		echo "ERROR: $wl failed under bp_tool" >&2
		status=1
		continue
	fi
	if [ ! -s "$stats" ]; then
		echo "ERROR: $wl produced no stats file ($stats)" >&2
		status=1
		continue
	fi

	insts=$(stat "$stats" tool.instructions)
	total=$(stat "$stats" tool.cf_events)

	# Category table, with one mispredictions column per simulator.
	printf "  %-10s %12s %12s" category count taken
	for k in ${SIM_KEYS[@]+"${SIM_KEYS[@]}"}; do
		printf " %16s" "$k.miss"
	done
	echo
	for c in "${CATEGORIES[@]}"; do
		printf "  %-10s %12s %12s" "$c" "$(stat "$stats" "tool.$c.count")" "$(stat "$stats" "tool.$c.taken")"
		for k in ${SIM_KEYS[@]+"${SIM_KEYS[@]}"}; do
			printf " %16s" "$(stat "$stats" "$k.$c.mispred")"
		done
		echo
	done
	printf "  %-10s %12s %12s" TOTAL "$total" ""
	for k in ${SIM_KEYS[@]+"${SIM_KEYS[@]}"}; do
		printf " %16s" "$(stat "$stats" "$k.total_mispred")"
	done
	echo
	printf "  instructions: %s\n" "$insts"

	for k in ${SIM_KEYS[@]+"${SIM_KEYS[@]}"}; do
		printf "  %s: cond_accuracy %s  cond_mpki %s  total_mpki %s  bits %s\n" "$k" \
				"$(stat "$stats" "$k.cond_accuracy")" "$(stat "$stats" "$k.cond_mpki")" \
				"$(stat "$stats" "$k.total_mpki")" "$(stat "$stats" "$k.bits.total")"
	done

	if [ -z "$insts" ] || [ "$insts" -lt "$MIN_INSTS" ]; then
		echo "ERROR: $wl ran ${insts:-0} instructions (fewer than $MIN_INSTS)" >&2
		status=1
	fi
	echo
done
exit $status
