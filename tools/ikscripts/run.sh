#!/bin/bash
# Runs one in-app posing script and builds a contact sheet of its shots.
#   POSESTUDIO_TEST_FIGURE=<figure preset> tools/ikscripts/run.sh <script.txt> <outdir> [NAME=value ...]
# Extra NAME=value arguments are passed to the app as environment (IK_… probes for an A/B).
# POSESTUDIO_EXE overrides the executable (default: build/Release/PoseStudio.exe).
#
# <outdir> is the RUN's folder, and a run starts from an empty one: what an earlier run left
# there is removed first — its ui/ and render/ pictures, the projects and materials a script
# saved, a texture store — or this run's file checks (`exists.`, `count.`, `matfile.copied`)
# would read the last run's files. Only an earlier run's folder is emptied (it holds a log.txt):
# a folder that is not empty and is no run's is REFUSED and left as it is, and so is one that
# holds what this run reads (the figure, the script, POSESTUDIO_IK_SCRIPT_ASSETS). Keep a
# script's inputs in the assets folder, never in the output folder.
#
# The exit code is the app's — the number of `expect` lines that failed — when the script reached
# its end. A run that did NOT reach it never exits 0: the app's own code where it has one (a
# crash's), else 101, the code the app itself ends with when it cannot read the script. 102: the
# output folder was refused and nothing was run.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
exe="${POSESTUDIO_EXE:-$here/../../build/Release/PoseStudio.exe}"
if [ -z "${POSESTUDIO_TEST_FIGURE:-}" ]; then
    echo "set POSESTUDIO_TEST_FIGURE to a figure preset" >&2
    exit 2
fi
script="$1"; out="$2"; shift 2

# --- the run's folder: empty before the app starts (see the header) ---
# A folder's absolute path, lower-cased (a Windows path is the same folder whatever its case);
# empty where there is no such folder.
canon() { ( cd "$1" 2>/dev/null && pwd -P | tr '[:upper:]' '[:lower:]' ); }
# Whether folder $1 is, or holds, folder $2 (both as canon gives them).
holds() { [ -n "$2" ] && case "$2/" in "$1"/*) return 0 ;; esac; return 1; }
is_empty() { [ -z "$(ls -A "$1" 2>/dev/null)" ]; }
refuse() {
    echo "run.sh: $1" >&2
    echo "run.sh: ${2:-nothing was run, and nothing in $out was changed.}" >&2
    exit 102
}
mkdir -p "$out"
if ! is_empty "$out"; then
    if [ ! -e "$out/log.txt" ] && [ ! -e "$out/unfinished.txt" ] && [ ! -e "$out/raw.txt" ]; then
        refuse "$out is not empty and no earlier run made it (it holds no log.txt). A run starts from an empty folder: name another, or empty this one."
    fi
    folder="$(canon "$out")"
    case "$folder" in
        ""|/|/?) refuse "$out is no folder to empty." ;;
    esac
    if [ "$folder" = "$(canon "${HOME:-/}")" ]; then
        refuse "$out is the home folder."
    fi
    # (The assets folder is named in the environment, or among this run's NAME=value arguments.)
    assets="${POSESTUDIO_IK_SCRIPT_ASSETS:-}"
    for argument in "$@"; do
        case "$argument" in POSESTUDIO_IK_SCRIPT_ASSETS=*) assets="${argument#*=}" ;; esac
    done
    for input in "$here" "$(dirname "$script")" "$(dirname "$POSESTUDIO_TEST_FIGURE")" "$(dirname "$exe")" ${assets:+"$assets"}; do
        if holds "$folder" "$(canon "$input")"; then
            refuse "$out is an earlier run's folder, but it holds $input, which this run reads. Name another output folder."
        fi
    done
    # (In a subshell: the globbing options are this one command's.)
    ( shopt -s dotglob nullglob; rm -rf -- "$out"/* )
    if ! is_empty "$out"; then
        refuse "the earlier run's files in $out could not all be removed (is one of them open in another program?)." "nothing was run."
    fi
fi

env "$@" POSESTUDIO_IK_SCRIPT="$script" POSESTUDIO_IK_SCRIPT_OUT="$out" POSESTUDIO_NO_PING=1 \
    POSESTUDIO_NO_UPDATE_CHECK=1 QT_LOGGING_TO_CONSOLE=1 "$exe" "$POSESTUDIO_TEST_FIGURE" > "$out/raw.txt" 2>&1
code=$?
grep -E "\[ikscript\]" "$out/raw.txt" > "$out/log.txt"
if grep -q "\[ikscript\] done" "$out/log.txt"; then
    rm -f "$out/raw.txt" "$out/unfinished.txt"
else
    # The app went away mid-script (a crash, a lost device, a window someone closed): keep what it
    # said, and how it ended, for the post-mortem.
    mv "$out/raw.txt" "$out/unfinished.txt"
    echo "DID NOT FINISH (exit code $code): the app's whole output is in $out/unfinished.txt"
    # (The app sets its exit code only at the script's end; a window closed by hand ends it
    # through Qt, with 0. An unfinished run is never a pass.)
    if [ "$code" -eq 0 ]; then code=101; fi
fi
grep -E "\]   \[|EXPECT|picked|began no drag|unknown|NOT SAVED|SHOT FAILED|\] done" "$out/log.txt"
python "$here/montage.py" "$out" "$out/sheet.png" "${SHEET_COLS:-4}" "${SHEET_CROP:-0.42}"
exit $code
