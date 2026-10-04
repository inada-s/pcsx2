#!/usr/bin/env bash
# Build (and optionally run) PCSX2 locally on Windows from Git Bash,
# mirroring the MSBuild path of the Windows CI. (AI-assisted)
#
#   ./build-local.sh                    # Release AVX2 build
#   ./build-local.sh --run              # build, then launch
#   ./build-local.sh --config Devel     # Devel build (asserts + dev features)
#   ./build-local.sh --run-only         # launch the last build without building
#   ./build-local.sh --rebuild          # clean rebuild of PCSX2 (deps are kept)
#   ./build-local.sh --run -- -fastboot /c/games/foo.iso   # args after -- go to pcsx2-qt
#
# The first run builds the third-party dependencies into ./deps (Qt, SDL, ffmpeg, ...),
# which takes a long time. Requires Visual Studio (C++ workload), 7-Zip and Git for Windows.

set -euo pipefail

CONFIG=Release
SIMD=AVX2
RUN=0
RUN_ONLY=0
REBUILD=0
REBUILD_DEPS=0
SKIP_PATCHES=0
GAME_ARGS=()

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
	case "$1" in
		-c|--config) CONFIG="$2"; shift 2 ;;
		--sse4) SIMD=SSE4; shift ;;
		-r|--run) RUN=1; shift ;;
		--run-only) RUN_ONLY=1; shift ;;
		--rebuild) REBUILD=1; shift ;;
		--rebuild-deps) REBUILD_DEPS=1; shift ;;
		--skip-patches) SKIP_PATCHES=1; shift ;;
		-h|--help) usage ;;
		--) shift; GAME_ARGS=("$@"); break ;;
		*) echo "Unknown option: $1" >&2; usage 1 ;;
	esac
done

case "$CONFIG" in
	Release|Devel|Debug) ;;
	*) echo "--config must be Release, Devel or Debug" >&2; exit 1 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

step() { printf '\033[36m==> %s\033[0m\n' "$*"; }

# MSBuild configuration names: "Release", "Release AVX2", "Devel AVX2", ...
MSBUILD_CONFIG="$CONFIG"
EXE_NAME=pcsx2-qtx64
if [[ "$SIMD" == AVX2 ]]; then MSBUILD_CONFIG+=" AVX2"; EXE_NAME+=-avx2; fi
[[ "$CONFIG" == Devel ]] && EXE_NAME+=-dev
[[ "$CONFIG" == Debug ]] && EXE_NAME+=-dbg
EXE="$ROOT/bin/$EXE_NAME.exe"

if [[ $RUN_ONLY -eq 0 ]]; then
	# --- Visual Studio / MSBuild ---
	VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
	[[ -x "$VSWHERE" ]] || { echo "vswhere.exe not found. Install Visual Studio with the C++ workload." >&2; exit 1; }
	MSBUILD_WIN="$("$VSWHERE" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | head -n1 | tr -d '\r')"
	[[ -n "$MSBUILD_WIN" ]] || { echo "MSBuild.exe not found." >&2; exit 1; }
	MSBUILD="$(cygpath -u "$MSBUILD_WIN")"
	step "MSBuild: $MSBUILD_WIN"

	# --- Dependencies ---
	# Rebuild deps when the CI deps script or its patches change (same key idea as the CI cache).
	DEPS_SCRIPT=.github/workflows/scripts/windows/build-dependencies.bat
	DEPS_HASH="$(cat "$DEPS_SCRIPT" .github/workflows/scripts/common/*.patch | sha256sum | cut -d' ' -f1)"
	DEPS_STAMP=deps/.local-build-hash

	if [[ $REBUILD_DEPS -eq 1 || ! -f "$DEPS_STAMP" || "$(cat "$DEPS_STAMP")" != "$DEPS_HASH" ]]; then
		if [[ -d deps ]]; then
			step "Dependencies are out of date; rebuilding deps (this takes a long time)"
			# Downloaded archives in deps-build are kept; the bat re-extracts each one.
			rm -rf deps
		else
			step "Building dependencies into ./deps (first time only, this takes a long time)"
		fi
		# cmd mis-handles "call :label" in LF-only batch files and silently skips lines
		# (core.autocrlf=input checks the bat out as LF), so run a CRLF copy next to the
		# original - the bat locates its patches and the repo root via %~dp0.
		DEPS_SCRIPT_CRLF="${DEPS_SCRIPT%.bat}.local-crlf.bat"
		trap 'rm -f "$ROOT/$DEPS_SCRIPT_CRLF"' EXIT
		sed 's/\r$//; s/$/\r/' "$DEPS_SCRIPT" > "$DEPS_SCRIPT_CRLF"
		# Give the bat a plain Windows PATH, like CI has:
		#  - drop Git Bash / MSYS2 dirs: MSYS2's mingw cmake/ninja would shadow the VS ones
		#    (vcvars appends them) and its cmake breaks rc.exe calls (RC1107), and GNU tar
		#    cannot extract .zip;
		#  - System32 first for bsdtar/curl/find/sort, plus the VS Installer dir for vswhere;
		#  - Git's mingw64/bin last for a native GNU make (nv-codec-headers, x264, ffmpeg);
		#    it has no cmake/ninja, so it cannot shadow the VS ones.
		DEPS_PATH="$(printf '%s' "$PATH" | tr ':' '\n' \
			| grep -viE '^(/usr(/|$)|/bin$|/mingw64(/|$)|/opt(/|$)|/c/msys64(/|$)|/c/Program Files/Git/(usr|mingw64)(/|$)|'"$HOME"'/bin$)' \
			| paste -sd:)"
		DEPS_PATH="/c/Windows/System32:/c/Program Files (x86)/Microsoft Visual Studio/Installer:$DEPS_PATH:/c/Program Files/Git/mingw64/bin"
		# The full output goes to a log file: streaming ffmpeg's parallel make output to the
		# terminal stalled the whole build once. stdin is /dev/null so the bat's "pause" on
		# failure returns immediately instead of waiting for an invisible prompt.
		mkdir -p deps-build
		DEPS_LOG=deps-build/local-build.log
		: >"$DEPS_LOG" # must exist before tail starts
		step "Full log: $DEPS_LOG"
		PATH="$DEPS_PATH" DEBUG=0 BUILD_FFMPEG=1 \
			cmd.exe //c "$(cygpath -w "$ROOT/$DEPS_SCRIPT_CRLF")" </dev/null >"$DEPS_LOG" 2>&1 &
		DEPS_PID=$!
		# Ctrl+C would otherwise leave the detached Windows process tree running.
		trap 'taskkill //F //T //PID "$(cat /proc/$DEPS_PID/winpid 2>/dev/null)" >/dev/null 2>&1; exit 130' INT TERM
		# Show only the progress lines from the log.
		tail -n +1 -f --pid="$DEPS_PID" "$DEPS_LOG" 2>/dev/null \
			| grep --line-buffered -aE '^("Installing|"?Building|Downloading|Cleaning|Failed)' || true
		DEPS_RC=0
		wait "$DEPS_PID" || DEPS_RC=$?
		trap - INT TERM
		if [[ $DEPS_RC -ne 0 || ! -f deps/bin/Qt6Core.dll ]]; then
			echo "Dependency build failed (exit $DEPS_RC). Last lines of $DEPS_LOG:" >&2
			tail -n 40 "$DEPS_LOG" >&2
			exit 1
		fi
		echo "$DEPS_HASH" > "$DEPS_STAMP"
		# The intermediate build tree is large and no longer needed.
		rm -rf deps-build
	else
		step "Dependencies up to date"
	fi

	# --- Game patches (bin/resources/patches.zip, git-ignored) ---
	if [[ $SKIP_PATCHES -eq 0 && ! -f bin/resources/patches.zip ]]; then
		step "Downloading patches.zip"
		curl -fL -o bin/resources/patches.zip \
			https://github.com/PCSX2/pcsx2_patches/releases/latest/download/patches.zip
	fi

	# --- Build ---
	# Use -switch style, not /switch: MSYS would rewrite /p:... as a path.
	TARGET=Build
	[[ $REBUILD -eq 1 ]] && TARGET=Rebuild
	step "Building PCSX2 ($MSBUILD_CONFIG|x64, $TARGET)"
	"$MSBUILD" PCSX2_qt.sln -m -v:m -nologo "-t:$TARGET" "-p:Configuration=$MSBUILD_CONFIG" -p:Platform=x64
	step "Built: bin/$EXE_NAME.exe"
fi

if [[ $RUN -eq 1 || $RUN_ONLY -eq 1 ]]; then
	[[ -f "$EXE" ]] || { echo "bin/$EXE_NAME.exe not found. Build first." >&2; exit 1; }
	step "Launching $EXE_NAME.exe ${GAME_ARGS[*]:-}"
	# Detach so the shell is not tied to the emulator window.
	(cd "$ROOT/bin" && "$EXE" ${GAME_ARGS[@]+"${GAME_ARGS[@]}"} >/dev/null 2>&1 &)
fi
