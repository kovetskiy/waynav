# @file int/lib/config.sh
# @brief Shared configuration helpers for waynav integration tests.
# @description
#   Provides default environment loading, validation, and path helpers used by
#   the Ancient shell integration test suite.

# @description Load default integration-test environment variables.
# @noargs
# @stdout None.
# @stderr None.
# @exitcode 0 Always.
waynav:int:load-defaults() {
	export WAYNAV_INT_DOCKER=${WAYNAV_INT_DOCKER:-${DOCKER:-docker}}
	export WAYNAV_INT_SWAY_IMAGE=${WAYNAV_INT_SWAY_IMAGE:-waynav-sway:bookworm}
	export WAYNAV_INT_NIRI_IMAGE=${WAYNAV_INT_NIRI_IMAGE:-waynav-niri:arch}
	export WAYNAV_INT_SWAY_CONTAINER=${WAYNAV_INT_SWAY_CONTAINER:-waynav-sway-int}
	export WAYNAV_INT_NIRI_CONTAINER=${WAYNAV_INT_NIRI_CONTAINER:-waynav-niri-int}
	export WAYNAV_INT_SWAY_BUILD_DIR=${WAYNAV_INT_SWAY_BUILD_DIR:-build-docker}
	export WAYNAV_INT_NIRI_BUILD_DIR=${WAYNAV_INT_NIRI_BUILD_DIR:-build-niri-docker}
	export WAYNAV_INT_SOCKET_TIMEOUT=${WAYNAV_INT_SOCKET_TIMEOUT:-20}
	export WAYNAV_INT_READY_TIMEOUT=${WAYNAV_INT_READY_TIMEOUT:-10}
	export WAYNAV_INT_EXIT_TIMEOUT=${WAYNAV_INT_EXIT_TIMEOUT:-5}
	export WAYNAV_INT_BUILD_IMAGE=${WAYNAV_INT_BUILD_IMAGE:-1}
	export WAYNAV_INT_RUN_UNIT_TESTS=${WAYNAV_INT_RUN_UNIT_TESTS:-1}
	export WAYNAV_INT_KEEP_CONTAINER=${WAYNAV_INT_KEEP_CONTAINER:-0}
}

# @description Validate shared integration-test configuration.
# @noargs
# @stdout None.
# @stderr Diagnostic messages for invalid configuration.
# @exitcode 0 If configuration is valid.
# @exitcode 1 If required paths or numeric values are invalid.
waynav:int:validate-config() {
	:waynav:int:require-env WAYNAV_INT_DIR
	:waynav:int:require-env WAYNAV_INT_ROOT_DIR
	:waynav:int:validate-positive-integer \
		WAYNAV_INT_SOCKET_TIMEOUT "$WAYNAV_INT_SOCKET_TIMEOUT"
	:waynav:int:validate-positive-integer \
		WAYNAV_INT_READY_TIMEOUT "$WAYNAV_INT_READY_TIMEOUT"
	:waynav:int:validate-positive-integer \
		WAYNAV_INT_EXIT_TIMEOUT "$WAYNAV_INT_EXIT_TIMEOUT"
	:waynav:int:validate-boolean \
		WAYNAV_INT_BUILD_IMAGE "$WAYNAV_INT_BUILD_IMAGE"
	:waynav:int:validate-boolean \
		WAYNAV_INT_RUN_UNIT_TESTS "$WAYNAV_INT_RUN_UNIT_TESTS"
	:waynav:int:validate-boolean \
		WAYNAV_INT_KEEP_CONTAINER "$WAYNAV_INT_KEEP_CONTAINER"
	:waynav:int:validate-relative-build-dir \
		WAYNAV_INT_SWAY_BUILD_DIR "$WAYNAV_INT_SWAY_BUILD_DIR"
	:waynav:int:validate-relative-build-dir \
		WAYNAV_INT_NIRI_BUILD_DIR "$WAYNAV_INT_NIRI_BUILD_DIR"
}

# @description Require an executable command.
# @arg $1 string Command name or path.
# @stdout None.
# @stderr Diagnostic message when the command is missing.
# @exitcode 0 If the command exists.
# @exitcode 1 If the command is not found.
waynav:int:require-command() {
	local name=$1

	if ! command -v -- "$name" >/dev/null 2>&1; then
		printf '[int] error: required command not found: %s\n' "$name" >&2
		return 1
	fi
}

# @description Return the Docker image name for a compositor flavor.
# @arg $1 string Flavor: sway or niri.
# @stdout Docker image tag.
# @stderr Diagnostic message for unknown flavors.
# @exitcode 0 If the flavor is known.
# @exitcode 1 If the flavor is unknown.
waynav:int:image-for() {
	local flavor=$1

	case "$flavor" in
	sway)
		printf '%s\n' "$WAYNAV_INT_SWAY_IMAGE"
		;;
	niri)
		printf '%s\n' "$WAYNAV_INT_NIRI_IMAGE"
		;;
	*)
		printf '[int] error: unknown image flavor: %s\n' "$flavor" >&2
		return 1
		;;
	esac
}

# @description Return the Dockerfile path for a compositor flavor.
# @arg $1 string Flavor: sway or niri.
# @stdout Dockerfile path.
# @stderr Diagnostic message for unknown flavors.
# @exitcode 0 If the flavor is known.
# @exitcode 1 If the flavor is unknown.
waynav:int:dockerfile-for() {
	local flavor=$1

	case "$flavor" in
	sway)
		printf '%s/sway.Dockerfile\n' "$WAYNAV_INT_DIR"
		;;
	niri)
		printf '%s/niri.Dockerfile\n' "$WAYNAV_INT_DIR"
		;;
	*)
		printf '[int] error: unknown Dockerfile flavor: %s\n' "$flavor" >&2
		return 1
		;;
	esac
}

# @description Return the Meson build directory for a compositor flavor.
# @arg $1 string Flavor: sway or niri.
# @stdout Build directory path used inside the container.
# @stderr Diagnostic message for unknown flavors.
# @exitcode 0 If the flavor is known.
# @exitcode 1 If the flavor is unknown.
waynav:int:build-dir-for() {
	local flavor=$1

	case "$flavor" in
	sway)
		printf '%s\n' "$WAYNAV_INT_SWAY_BUILD_DIR"
		;;
	niri)
		printf '%s\n' "$WAYNAV_INT_NIRI_BUILD_DIR"
		;;
	*)
		printf '[int] error: unknown build flavor: %s\n' "$flavor" >&2
		return 1
		;;
	esac
}

# @description Return the waynav binary path inside a container.
# @arg $1 string Build directory path inside the container.
# @stdout Absolute or working-directory-relative waynav binary path.
# @stderr None.
# @exitcode 0 Always.
waynav:int:waynav-path() {
	local build_dir=$1

	case "$build_dir" in
	/*)
		printf '%s/waynav\n' "$build_dir"
		;;
	*)
		printf '/src/%s/waynav\n' "$build_dir"
		;;
	esac
}

# @description Return the host result directory for a testcase flavor.
# @arg $1 string Result flavor, such as sway or niri.
# @stdout Host path under the current tests.sh temporary directory.
# @stderr None.
# @exitcode 0 Always.
waynav:int:host-result-dir() {
	local flavor=$1

	printf '%s/%s\n' "$(tests:get-tmp-dir)" "$flavor"
}

# @description Return the container result directory for a testcase flavor.
# @arg $1 string Result flavor, such as sway or niri.
# @stdout Container path under /tmp/waynav-int.
# @stderr None.
# @exitcode 0 Always.
waynav:int:container-result-dir() {
	local flavor=$1

	printf '/tmp/waynav-int/%s\n' "$flavor"
}

:waynav:int:require-env() {
	local name=$1

	if [[ -z ${!name:-} ]]; then
		printf '[int] error: required environment is empty: %s\n' "$name" >&2
		return 1
	fi
}

:waynav:int:validate-positive-integer() {
	local name=$1
	local value=$2

	if [[ ! $value =~ ^[1-9][0-9]*$ ]]; then
		printf '[int] error: %s must be a positive integer: %s\n' \
			"$name" "$value" >&2
		return 1
	fi
}

:waynav:int:validate-boolean() {
	local name=$1
	local value=$2

	case "$value" in
	0 | 1)
		;;
	*)
		printf '[int] error: %s must be 0 or 1: %s\n' "$name" "$value" >&2
		return 1
		;;
	esac
}

:waynav:int:validate-relative-build-dir() {
	local name=$1
	local value=$2

	case "$value" in
	'' | /* | .* | *..*)
		printf '[int] error: %s must be a plain relative path: %s\n' \
			"$name" "$value" >&2
		return 1
		;;
	esac
}
