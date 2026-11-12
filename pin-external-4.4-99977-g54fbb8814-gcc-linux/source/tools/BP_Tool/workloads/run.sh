#!/usr/bin/env bash
# Run each workload under bp_tool and print its control transfer counts by category.
# Usage: [PIN_ROOT=/path/to/pin-kit] ./run.sh [workload ...]   (default: all workloads)
set -u

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

if [ $# -gt 0 ]; then
	WORKLOADS=("$@")
else
	WORKLOADS=(loop sort recursion dispatch)
fi

# Build the pintool and the workloads.
make -s -C "$TOOL_DIR" PIN_ROOT="$PIN_ROOT" obj-intel64/bp_tool.so || {
	echo "ERROR: failed to build bp_tool" >&2
	exit 1
}
make -s -C "$WL_DIR" "${WORKLOADS[@]}" || {
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
	if ! "$PIN" -t "$TOOL" -o "$stats" -- "$WL_DIR/$wl"; then
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
	printf "  %-10s %12s %12s\n" category count taken
	for c in "${CATEGORIES[@]}"; do
		printf "  %-10s %12s %12s\n" "$c" "$(stat "$stats" "tool.$c.count")" "$(stat "$stats" "tool.$c.taken")"
	done
	printf "  %-10s %12s\n" TOTAL "$total"
	printf "  instructions: %s\n" "$insts"

	if [ -z "$insts" ] || [ "$insts" -lt "$MIN_INSTS" ]; then
		echo "ERROR: $wl ran ${insts:-0} instructions (fewer than $MIN_INSTS)" >&2
		status=1
	fi
	echo
done
exit $status
