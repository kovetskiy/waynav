#!/usr/bin/env bash
# @file int/lib/run-nested-niri-smoke.sh
# @brief Exercise waynav against nested niri in Docker.
# @description
#   Starts a headless Sway parent compositor, runs niri nested as a Wayland
#   client, verifies niri's child Wayland socket and protocols, then delegates
#   the waynav overlay check to run-waynav-smoke.sh.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly SCRIPT_DIR

readonly DEFAULT_SOCKET_TIMEOUT='20'
readonly DEFAULT_READY_TIMEOUT='10'
readonly DEFAULT_EXIT_TIMEOUT='5'

:usage() {
	cat <<EOF
Usage:
    run-nested-niri-smoke.sh --waynav PATH --result-dir DIR [options]

Options:
    --waynav PATH          waynav binary to execute.
    --result-dir DIR       Directory for compositor and waynav logs.
    --socket-timeout SEC   Seconds to wait for compositor sockets.
                           Default: ${DEFAULT_SOCKET_TIMEOUT}
    --ready-timeout SEC    Seconds to wait for waynav overlay readiness.
                           Default: ${DEFAULT_READY_TIMEOUT}
    --exit-timeout SEC     Seconds to wait for waynav to exit.
                           Default: ${DEFAULT_EXIT_TIMEOUT}
    -h, --help             Show this help.
EOF
}

:log() {
	printf '[niri-smoke] %s\n' "$*" >&2
}

:error() {
	printf '[niri-smoke] error: %s\n' "$*" >&2
	return 1
}

# shellcheck disable=SC2034 # Nameref assignments update caller variables.
:parse-opts() {
	local -n waynav_ref=$1
	local -n result_dir_ref=$2
	local -n socket_timeout_ref=$3
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
		--result-dir)
			(($# > 1)) || return 2
			result_dir_ref=$2
			shift 2
			;;
		--result-dir=*)
			result_dir_ref=${1#*=}
			shift
			;;
		--socket-timeout)
			(($# > 1)) || return 2
			socket_timeout_ref=$2
			shift 2
			;;
		--socket-timeout=*)
			socket_timeout_ref=${1#*=}
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
	local name=$1
	local pid=$2
	local signal=${3:-TERM}

	if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
		:log "stopping $name"
		kill "-$signal" "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	fi
}

:cleanup-all() {
	local status=$?
	local sway_pid=$1
	local niri_pid=${2:-}

	:cleanup-process niri "$niri_pid"
	:cleanup-process sway "$sway_pid"
	exit "$status"
}

:write-sway-config() {
	local path=$1

	cat >"$path" <<'EOF'
xwayland disable
output HEADLESS-1 resolution 1280x720
EOF
}

:wait-file-socket() {
	local path=$1
	local pid=$2
	local timeout=$3
	local elapsed=0

	while ((elapsed < timeout)); do
		if [[ -S $path ]]; then
			return 0
		fi

		if ! kill -0 "$pid" 2>/dev/null; then
			return 1
		fi

		sleep 1
		((elapsed += 1))
	done

	return 1
}

:wait-niri-socket() {
	local log_file=$1
	local runtime_dir=$2
	local pid=$3
	local timeout=$4
	local elapsed=0
	local socket=''

	while ((elapsed < timeout)); do
		socket=$(sed -n 's/.*listening on Wayland socket: //p' \
			"$log_file" | tail -1 | tr -d '\r' || true)
		if [[ -n $socket && -S $runtime_dir/$socket ]]; then
			printf '%s\n' "$socket"
			return 0
		fi

		if ! kill -0 "$pid" 2>/dev/null; then
			return 1
		fi

		sleep 1
		((elapsed += 1))
	done

	return 1
}

:write-empty-niri-config() {
	local path=$1

	: >"$path"
}

:verify-protocols() {
	local info_file=$1
	local missing_file=$2
	local proto=''
	local missing=0

	: >"$missing_file"
	for proto in \
		zwlr_layer_shell_v1 \
		zwlr_virtual_pointer_manager_v1 \
		wp_fractional_scale_manager_v1 \
		wp_viewporter \
		zxdg_output_manager_v1 \
		zwp_virtual_keyboard_manager_v1; do
		if ! grep -q -- "$proto" "$info_file"; then
			printf '%s\n' "$proto" >>"$missing_file"
			missing=1
		fi
	done

	if ((missing)); then
		:error 'niri did not advertise required protocols'
		return 1
	fi
}

:run-smoke() {
	local waynav=$1
	local result_dir=$2
	local socket_timeout=$3
	local ready_timeout=$4
	local exit_timeout=$5
	local runtime_dir=/tmp/xdg
	local sway_log="$result_dir/sway.log"
	local niri_log="$result_dir/niri.log"
	local niri_info="$result_dir/niri-wayland-info.txt"
	local niri_socket_file="$result_dir/niri-socket"
	local missing_protocols="$result_dir/missing-protocols"
	local sway_config="$result_dir/sway.conf"
	local niri_config="$result_dir/niri-empty.kdl"
	local niri_socket=''
	local sway_pid=''
	local niri_pid=''

	rm -rf -- "$result_dir"
	mkdir -p -- "$result_dir" "$runtime_dir"
	chmod 700 "$runtime_dir"
	:write-sway-config "$sway_config"
	:write-empty-niri-config "$niri_config"

	:log 'starting headless Sway parent'
	export XDG_RUNTIME_DIR=$runtime_dir
	export WLR_BACKENDS=headless
	export WLR_LIBINPUT_NO_DEVICES=1
	export WLR_RENDERER=pixman
	sway --unsupported-gpu -d -c "$sway_config" >"$sway_log" 2>&1 &
	sway_pid=$!
	# shellcheck disable=SC2064 # Capture child PIDs for signal cleanup.
	trap ":cleanup-all $(printf '%q' "$sway_pid") ''" EXIT INT TERM

	if ! :wait-file-socket "$runtime_dir/wayland-1" \
		"$sway_pid" "$socket_timeout"; then
		:error 'Sway parent did not create wayland-1'
		return 1
	fi

	:log 'starting nested niri'
	unset WLR_BACKENDS WLR_LIBINPUT_NO_DEVICES WLR_RENDERER
	export WAYLAND_DISPLAY=wayland-1
	export NIRI_DISABLE_SYSTEM_MANAGER_NOTIFY=1
	export RUST_LOG=niri=debug,smithay=warn
	niri -c "$niri_config" >"$niri_log" 2>&1 &
	niri_pid=$!
	# shellcheck disable=SC2064 # Capture child PIDs for signal cleanup.
	trap ":cleanup-all $(printf '%q' "$sway_pid") $(printf '%q' "$niri_pid")" EXIT INT TERM

	if ! niri_socket=$(:wait-niri-socket \
		"$niri_log" "$runtime_dir" "$niri_pid" "$socket_timeout"); then
		:error 'niri did not create a child Wayland socket'
		return 1
	fi
	printf '%s\n' "$niri_socket" >"$niri_socket_file"

	:log "niri socket: $niri_socket"
	WAYLAND_DISPLAY=$niri_socket wayland-info >"$niri_info"
	:verify-protocols "$niri_info" "$missing_protocols"

	WAYLAND_DISPLAY=$niri_socket \
		bash "$SCRIPT_DIR/run-waynav-smoke.sh" \
		--waynav "$waynav" \
		--display "$niri_socket" \
		--result-dir "$result_dir" \
		--ready-timeout "$ready_timeout" \
		--exit-timeout "$exit_timeout"

	:log 'passed'
}

:main() {
	local waynav=''
	local result_dir=''
	local socket_timeout=$DEFAULT_SOCKET_TIMEOUT
	local ready_timeout=$DEFAULT_READY_TIMEOUT
	local exit_timeout=$DEFAULT_EXIT_TIMEOUT
	local show_help=0

	if ! :parse-opts \
		waynav \
		result_dir \
		socket_timeout \
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
	:validate-required --result-dir "$result_dir"
	:validate-timeout --socket-timeout "$socket_timeout"
	:validate-timeout --ready-timeout "$ready_timeout"
	:validate-timeout --exit-timeout "$exit_timeout"

	if [[ ! -x $waynav ]]; then
		:error "waynav binary is not executable: $waynav"
		return 1
	fi

	:run-smoke "$waynav" "$result_dir" "$socket_timeout" \
		"$ready_timeout" "$exit_timeout"
}

:main "$@"
