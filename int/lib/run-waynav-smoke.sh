#!/usr/bin/env bash
# @file int/lib/run-waynav-smoke.sh
# @brief Exercise waynav against an existing Wayland compositor.
# @description
#   Runs waynav with a temporary keynav-style config, exits it through wtype,
#   and leaves stdout and stderr logs in a result directory for the outer
#   Ancient shell testcase to assert.

set -euo pipefail

readonly DEFAULT_READY_TIMEOUT='10'
readonly DEFAULT_EXIT_TIMEOUT='5'
readonly WAYNAV_LOG_LEVEL='debug'

:usage() {
	cat <<EOF
Usage:
    run-waynav-smoke.sh --waynav PATH --display NAME --result-dir DIR [options]

Options:
    --waynav PATH         waynav binary to execute.
    --display NAME        WAYLAND_DISPLAY value for waynav and wtype.
    --result-dir DIR      Directory for waynav.out and waynav.log.
    --ready-timeout SEC   Seconds to wait for overlay readiness.
                          Default: ${DEFAULT_READY_TIMEOUT}
    --exit-timeout SEC    Seconds to wait for waynav to exit.
                          Default: ${DEFAULT_EXIT_TIMEOUT}
    -h, --help            Show this help.
EOF
}

:log() {
	printf '[waynav-smoke] %s\n' "$*" >&2
}

:error() {
	printf '[waynav-smoke] error: %s\n' "$*" >&2
	return 1
}

# shellcheck disable=SC2034 # Nameref assignments update caller variables.
:parse-opts() {
	local -n waynav_ref=$1
	local -n display_ref=$2
	local -n result_dir_ref=$3
	local -n ready_timeout_ref=$4
	local -n exit_timeout_ref=$5
	local -n show_help_ref=$6

	shift 6
	while (($# > 0)); do
		case "$1" in
		--waynav)
			(($# > 1)) || return 2
			waynav_ref=$2
			shift 2
			;;
		--waynav=*)
			waynav_ref=${1#*=}
			shift
			;;
		--display)
			(($# > 1)) || return 2
			display_ref=$2
			shift 2
			;;
		--display=*)
			display_ref=${1#*=}
			shift
			;;
		--result-dir)
			(($# > 1)) || return 2
			result_dir_ref=$2
			shift 2
			;;
		--result-dir=*)
			result_dir_ref=${1#*=}
			shift
			;;
		--ready-timeout)
			(($# > 1)) || return 2
			ready_timeout_ref=$2
			shift 2
			;;
		--ready-timeout=*)
			ready_timeout_ref=${1#*=}
			shift
			;;
		--exit-timeout)
			(($# > 1)) || return 2
			exit_timeout_ref=$2
			shift 2
			;;
		--exit-timeout=*)
			exit_timeout_ref=${1#*=}
			shift
			;;
		-h | --help)
			show_help_ref=1
			shift
			;;
		*)
			:error "unexpected argument: $1"
			return 1
			;;
		esac
	done
}

:validate-required() {
	local label=$1
	local value=$2

	if [[ -z $value ]]; then
		:error "$label is required"
		return 1
	fi
}

:validate-timeout() {
	local label=$1
	local value=$2

	if [[ ! $value =~ ^[1-9][0-9]*$ ]]; then
		:error "$label must be a positive integer: $value"
		return 1
	fi
}

:cleanup-process() {
	local pid=$1

	if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
		kill -INT "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
}

:cleanup() {
	local status=$?
	local pid=$1

	:cleanup-process "$pid"
	exit "$status"
}

:write-waynavrc() {
	local path=$1

	cat >"$path" <<'EOF'
clear
super+semicolon start,grid 2x2
q cell-select 1,warp
w cell-select 2,warp
a cell-select 3,warp
s cell-select 4,warp
semicolon end
Return end
EOF
}

:process-running() {
	local pid=$1

	kill -0 "$pid" 2>/dev/null
}

:print-file() {
	local label=$1
	local path=$2

	printf '%s:\n' "$label" >&2
	if [[ -s $path ]]; then
		cat -- "$path" >&2
	else
		printf '  <empty>\n' >&2
	fi
}

:wait-log-matches() {
	local log_file=$1
	local pattern=$2
	local pid=$3
	local timeout=$4
	local elapsed=0

	while ((elapsed < timeout)); do
		if [[ -f $log_file ]] && grep -Eq -- "$pattern" "$log_file"; then
			return 0
		fi

		if ! :process-running "$pid"; then
			return 1
		fi

		sleep 1
		((elapsed += 1))
	done

	return 1
}

:wait-process-exit() {
	local pid=$1
	local timeout=$2
	local elapsed=0

	while ((elapsed < timeout)); do
		if ! :process-running "$pid"; then
			return 0
		fi

		sleep 1
		((elapsed += 1))
	done

	return 1
}

:run-smoke() {
	local waynav=$1
	local display=$2
	local result_dir=$3
	local ready_timeout=$4
	local exit_timeout=$5
	local waynavrc="$result_dir/waynavrc"
	local out_file="$result_dir/waynav.out"
	local log_file="$result_dir/waynav.log"
	local pid=''
	local status=0

	mkdir -p -- "$result_dir"
	rm -f -- "$waynavrc" "$out_file" "$log_file"
	:write-waynavrc "$waynavrc"
	:log 'starting waynav'

	export WAYLAND_DISPLAY=$display
	export WAYNAV_LOG=$WAYNAV_LOG_LEVEL
	export WAYNAV_LOG_COLOR=0

	"$waynav" -l "$WAYNAV_LOG_LEVEL" -c "$waynavrc" \
		>"$out_file" 2>"$log_file" &
	pid=$!

	# shellcheck disable=SC2064 # Capture the child PID before locals unwind.
	trap ":cleanup $(printf '%q' "$pid")" EXIT INT TERM

	if ! :wait-log-matches \
		"$log_file" 'overlay created:' "$pid" "$ready_timeout"; then
		:print-file 'waynav stdout' "$out_file"
		:print-file 'waynav log' "$log_file"
		:error 'overlay did not become ready'
		return 1
	fi

	:log 'sending exit key'
	wtype ';'

	if ! :wait-process-exit "$pid" "$exit_timeout"; then
		:cleanup-process "$pid"
		:print-file 'waynav stdout' "$out_file"
		:print-file 'waynav log' "$log_file"
		:error 'waynav did not exit after semicolon'
		return 1
	fi

	wait "$pid" || status=$?
	trap - EXIT INT TERM

	if ((status != 0)); then
		:print-file 'waynav stdout' "$out_file"
		:print-file 'waynav log' "$log_file"
		:error "waynav exited with status $status"
		return "$status"
	fi

	:log 'passed'
}

:main() {
	local waynav=''
	local display=''
	local result_dir=''
	local ready_timeout=$DEFAULT_READY_TIMEOUT
	local exit_timeout=$DEFAULT_EXIT_TIMEOUT
	local show_help=0

	if ! :parse-opts \
		waynav \
		display \
		result_dir \
		ready_timeout \
		exit_timeout \
		show_help \
		"$@"; then
		:usage >&2
		return 1
	fi

	if ((show_help)); then
		:usage
		return 0
	fi

	:validate-required --waynav "$waynav"
	:validate-required --display "$display"
	:validate-required --result-dir "$result_dir"
	:validate-timeout --ready-timeout "$ready_timeout"
	:validate-timeout --exit-timeout "$exit_timeout"

	if [[ ! -x $waynav ]]; then
		:error "waynav binary is not executable: $waynav"
		return 1
	fi

	:run-smoke "$waynav" "$display" "$result_dir" \
		"$ready_timeout" "$exit_timeout"
}

:main "$@"
