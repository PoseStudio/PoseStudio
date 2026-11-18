#!/bin/bash
# Runs every in-app posing script in this folder as a TEST and reports what changed.
#   POSESTUDIO_TEST_FIGURE=<figure preset> tools/ikscripts/run_all.sh <outdir> [--accept] [NAME=value ...]
#
# Per script: the app runs it (run.sh), its `expect` lines pass or fail (the app's exit code is the
# number that failed), a contact sheet is built, and — when POSESTUDIO_IKSHOTS_REF names a folder of
# reference shots — every shot is compared with its reference (compare.py): the ones that LOOK
# different are listed and laid out beside their references in <outdir>/<script>/changes.png.
# --accept makes this run's shots the new references (after you have looked at the changes) — for
# the scripts that PASSED: one that failed an expectation or did not finish keeps its references.
# Extra NAME=value arguments reach the app as environment (IK_… probes for an A/B run).
# Each script's folder <outdir>/<script> starts empty: run.sh removes what an earlier run left in
# it, and refuses a folder that is not empty and is no run's (the script then counts as failed).
# The exit code is the number of scripts that failed an expectation or did not finish.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
# (Said once, here: run.sh would say it per script and run nothing, and the summary below would
# then report whatever verdicts an EARLIER run left in the folders.)
if [ -z "${POSESTUDIO_TEST_FIGURE:-}" ]; then
    echo "set POSESTUDIO_TEST_FIGURE to a figure preset" >&2
    exit 2
fi
out="$1"; shift
accept=0
probes=()
for a in "$@"; do
    if [ "$a" = "--accept" ]; then accept=1; else probes+=("$a"); fi
done
ref="${POSESTUDIO_IKSHOTS_REF:-}"
failed=0
summary=()
for script in "$here"/*.txt; do
    name="$(basename "$script" .txt)"
    echo "=== $name"
    bash "$here/run.sh" "$script" "$out/$name" ${probes[@]+"${probes[@]}"} | grep -E "EXPECT FAILED|DID NOT FINISH|began no drag|unknown|NOT SAVED|SHOT FAILED|^sheet"
    ran=${PIPESTATUS[0]}
    # (102: run.sh refused the folder and ran nothing — a log.txt lying in it is an EARLIER run's,
    # and its verdict is not this run's.)
    verdict=""
    if [ "$ran" -ne 102 ]; then
        verdict="$(grep -E "\[ikscript\] done" "$out/$name/log.txt" | sed 's/.*done: //')"
    fi
    passed=0
    if [ -z "$verdict" ]; then
        verdict="DID NOT FINISH"
        failed=$((failed + 1))
    elif ! echo "$verdict" | grep -q " 0 FAILED"; then
        failed=$((failed + 1))
    else
        passed=1
    fi
    summary+=("$name: $verdict")
    # (A refused folder holds no shots of this run, and is not written to: no comparison.)
    if [ -n "$ref" ] && [ "$ran" -ne 102 ]; then
        if [ "$accept" = 1 ] && [ "$passed" = 1 ]; then
            python "$here/compare.py" "$out/$name" "$ref/$name" --accept
        elif [ "$accept" = 1 ]; then
            # The shots of a script that failed an expectation, or stopped half way, are not what
            # the next run should be compared with: its references stay, and are compared as usual.
            echo "  NOT ACCEPTED ($verdict): the reference shots of $name stay as they were"
            if [ -d "$ref/$name" ]; then
                python "$here/compare.py" "$out/$name" "$ref/$name"
            fi
        elif [ -d "$ref/$name" ]; then
            python "$here/compare.py" "$out/$name" "$ref/$name"
        else
            echo "  (no reference shots yet: run with --accept to keep these)"
        fi
    fi
done
echo "=== summary"
for line in "${summary[@]}"; do echo "  $line"; done
exit $failed
