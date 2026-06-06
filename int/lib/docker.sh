# @file int/lib/docker.sh
# @brief Docker helpers for waynav integration tests.
# @description
#   Builds compositor images, prepares waynav inside containers, starts
#   compositor containers, and runs container-local smoke scripts for the
#   integration testcases.

# @description Build a Docker image for a compositor flavor unless disabled.
# @arg $1 string Flavor: sway or niri.
# @stdout None.
# @stderr Docker build diagnostics on failure.
# @exitcode 0 If the image exists or was built.
# @exitcode 88 If the harness assertion fails.
waynav:int:build-image() {
	local flavor=$1
	local image=''
	local dockerfile=''

	if ((WAYNAV_INT_BUILD_IMAGE == 0)); then
		printf '[int] reusing %s image\n' "$flavor" >&2
		return 0
	fi

	image=$(waynav:int:image-for "$flavor")
	dockerfile=$(waynav:int:dockerfile-for "$flavor")

	printf '[int] building %s image: %s\n' "$flavor" "$image" >&2
	tests:eval "$WAYNAV_INT_DOCKER" build \
		-f "$dockerfile" \
		-t "$image" \
		"$WAYNAV_INT_DIR"
	:waynav:int:print-last-output-on-failure
	tests:assert-success
}

# @description Build waynav or run Meson unit tests inside a Docker image.
# @arg $1 string Flavor: sway or niri.
# @stdout None.
# @stderr Build or test diagnostics on failure.
# @exitcode 0 If waynav was built and optional unit tests passed.
# @exitcode 88 If the harness assertion fails.
waynav:int:prepare-build() {
	local flavor=$1
	local image=''
	local build_dir=''
	local action='build'

	image=$(waynav:int:image-for "$flavor")
	build_dir=$(waynav:int:build-dir-for "$flavor")

	if ((WAYNAV_INT_RUN_UNIT_TESTS)); then
		action='test'
		printf '[int] running unit tests in %s image\n' "$flavor" >&2
		# shellcheck disable=SC2016 # The command is expanded in the container.
		tests:eval "$WAYNAV_INT_DOCKER" run --rm \
			-v "$WAYNAV_INT_ROOT_DIR:/src" \
			-w /src \
			"$image" \
			bash -lc 'rm -rf -- "$1" && BUILDDIR="$1" make test' \
			bash "$build_dir"
	else
		printf '[int] building waynav in %s image\n' "$flavor" >&2
		# shellcheck disable=SC2016 # The command is expanded in the container.
		tests:eval "$WAYNAV_INT_DOCKER" run --rm \
			-v "$WAYNAV_INT_ROOT_DIR:/src" \
			-w /src \
			"$image" \
			bash -lc 'BUILDDIR="$1" make build' \
			bash "$build_dir"
	fi

	:waynav:int:print-last-output-on-failure
	tests:assert-success
	printf '[int] %s complete in %s image\n' "$action" "$flavor" >&2
}

:waynav:int:remove-container() {
	local container_name=$1

	"$WAYNAV_INT_DOCKER" rm -f "$container_name" >/dev/null 2>&1 || true
}

# @description Remove a named container unless --keep-container was requested.
# @arg $1 string Container name.
# @stdout None.
# @stderr Cleanup diagnostics.
# @exitcode 0 Always.
waynav:int:cleanup-container() {
	local container_name=$1

	if ((WAYNAV_INT_KEEP_CONTAINER)); then
		printf '[int] keeping container: %s\n' "$container_name" >&2
		return 0
	fi

	:waynav:int:remove-container "$container_name"
}

# @description Start a headless Sway compositor container.
# @noargs
# @stdout None.
# @stderr Docker diagnostics on failure.
# @exitcode 0 If the container was started.
# @exitcode 88 If the harness assertion fails.
waynav:int:start-sway() {
	local image=''
	local result_root=''
	local container_id=''

	image=$(waynav:int:image-for sway)
	result_root=$(tests:get-tmp-dir)

	:waynav:int:remove-container "$WAYNAV_INT_SWAY_CONTAINER"

	printf '[int] starting Sway container: %s\n' \
		"$WAYNAV_INT_SWAY_CONTAINER" >&2
	tests:eval "$WAYNAV_INT_DOCKER" run -d \
		--name "$WAYNAV_INT_SWAY_CONTAINER" \
		-v "$WAYNAV_INT_ROOT_DIR:/src" \
		-v "$result_root:/tmp/waynav-int" \
		-w /src \
		--security-opt seccomp=unconfined \
		"$image" \
		bash /src/int/lib/start-headless-sway.sh
	:waynav:int:print-last-output-on-failure
	tests:assert-success

	container_id=$(tests:get-stdout)
	printf '[int] Sway container started: %s\n' "$container_id" >&2
}

# @description Assert that a socket exists inside a running container.
# @arg $1 string Container name.
# @arg $2 string Socket path inside the container.
# @stdout None.
# @stderr Container logs if the socket does not appear.
# @exitcode 0 If the socket appears before the timeout.
# @exitcode 88 If the harness assertion fails.
waynav:int:assert-container-socket() {
	local container_name=$1
	local socket_path=$2
	local elapsed=0

	printf '[int] waiting for socket in %s: %s\n' \
		"$container_name" "$socket_path" >&2
	while ((elapsed < WAYNAV_INT_SOCKET_TIMEOUT)); do
		tests:eval "$WAYNAV_INT_DOCKER" exec "$container_name" \
			test -S "$socket_path"
		if [[ $(tests:get-exitcode) == 0 ]]; then
			tests:assert-success
			return 0
		fi

		if ! :waynav:int:container-running "$container_name"; then
			printf '[int] container exited before socket appeared: %s\n' \
				"$container_name" >&2
			waynav:int:print-container-logs "$container_name"
			tests:eval false
			tests:assert-success
			return 88
		fi

		sleep 1
		((elapsed += 1))
	done

	printf '[int] socket did not appear in %s: %s\n' \
		"$container_name" "$socket_path" >&2
	waynav:int:print-container-logs "$container_name"
	tests:assert-success
}

# @description Run the waynav overlay smoke script in the Sway container.
# @noargs
# @stdout None.
# @stderr Smoke diagnostics and waynav logs on failure.
# @exitcode 0 If the container-local smoke script succeeds.
# @exitcode 88 If the harness assertion fails.
waynav:int:run-sway-smoke() {
	local build_dir=''
	local waynav=''
	local result_dir=''

	build_dir=$(waynav:int:build-dir-for sway)
	waynav=$(waynav:int:waynav-path "$build_dir")
	result_dir=$(waynav:int:container-result-dir sway)

	printf '[int] running Sway overlay smoke\n' >&2
	tests:eval "$WAYNAV_INT_DOCKER" exec \
		-e XDG_RUNTIME_DIR=/tmp/xdg \
		-e WAYLAND_DISPLAY=wayland-1 \
		-e WAYNAV_LOG_COLOR=0 \
		"$WAYNAV_INT_SWAY_CONTAINER" \
		bash /src/int/lib/run-waynav-smoke.sh \
		--waynav "$waynav" \
		--display wayland-1 \
		--result-dir "$result_dir" \
		--ready-timeout "$WAYNAV_INT_READY_TIMEOUT" \
		--exit-timeout "$WAYNAV_INT_EXIT_TIMEOUT"
	:waynav:int:print-sway-smoke-on-failure
	tests:assert-success
}

# @description Run the nested niri smoke script in a Docker container.
# @noargs
# @stdout None.
# @stderr Smoke diagnostics and nested compositor logs on failure.
# @exitcode 0 If the nested niri smoke script succeeds.
# @exitcode 88 If the harness assertion fails.
waynav:int:run-niri-smoke() {
	local image=''
	local build_dir=''
	local waynav=''
	local result_root=''
	local result_dir=''

	image=$(waynav:int:image-for niri)
	build_dir=$(waynav:int:build-dir-for niri)
	waynav=$(waynav:int:waynav-path "$build_dir")
	result_root=$(tests:get-tmp-dir)
	result_dir=$(waynav:int:container-result-dir niri)

	:waynav:int:remove-container "$WAYNAV_INT_NIRI_CONTAINER"

	printf '[int] running nested niri smoke\n' >&2
	tests:eval "$WAYNAV_INT_DOCKER" run \
		--name "$WAYNAV_INT_NIRI_CONTAINER" \
		-v "$WAYNAV_INT_ROOT_DIR:/src" \
		-v "$result_root:/tmp/waynav-int" \
		-w /src \
		--cap-add SYS_NICE \
		--security-opt seccomp=unconfined \
		"$image" \
		bash /src/int/lib/run-nested-niri-smoke.sh \
		--waynav "$waynav" \
		--result-dir "$result_dir" \
		--socket-timeout "$WAYNAV_INT_SOCKET_TIMEOUT" \
		--ready-timeout "$WAYNAV_INT_READY_TIMEOUT" \
		--exit-timeout "$WAYNAV_INT_EXIT_TIMEOUT"
	:waynav:int:print-niri-smoke-on-failure
	tests:assert-success
}

# @description Print container logs to stderr.
# @arg $1 string Container name.
# @stdout None.
# @stderr Docker logs, if available.
# @exitcode 0 Always.
waynav:int:print-container-logs() {
	local container_name=$1

	printf '%s container log:\n' "$container_name" >&2
	"$WAYNAV_INT_DOCKER" logs "$container_name" >&2 || true
}

:waynav:int:container-running() {
	local container_name=$1
	local state=''

	state=$("$WAYNAV_INT_DOCKER" inspect \
		-f '{{.State.Running}}' "$container_name" 2>/dev/null || true)
	[[ $state == true ]]
}

:waynav:int:print-last-output-on-failure() {
	if [[ $(tests:get-exitcode) == 0 ]]; then
		return 0
	fi

	printf 'command stdout:\n' >&2
	cat "$(tests:get-stdout-file)" >&2
	printf 'command stderr:\n' >&2
	cat "$(tests:get-stderr-file)" >&2
}

:waynav:int:print-sway-smoke-on-failure() {
	local result_dir=''

	if [[ $(tests:get-exitcode) == 0 ]]; then
		return 0
	fi

	result_dir=$(waynav:int:host-result-dir sway)
	:waynav:int:print-last-output-on-failure
	:waynav:int:print-result-dir "$result_dir"
	waynav:int:print-container-logs "$WAYNAV_INT_SWAY_CONTAINER"
}

:waynav:int:print-niri-smoke-on-failure() {
	local result_dir=''

	if [[ $(tests:get-exitcode) == 0 ]]; then
		return 0
	fi

	result_dir=$(waynav:int:host-result-dir niri)
	:waynav:int:print-last-output-on-failure
	:waynav:int:print-result-dir "$result_dir"
	waynav:int:print-container-logs "$WAYNAV_INT_NIRI_CONTAINER"
}

:waynav:int:print-result-dir() {
	local result_dir=$1
	local file=''

	if [[ ! -d $result_dir ]]; then
		printf 'result directory is missing: %s\n' "$result_dir" >&2
		return 0
	fi

	for file in \
		"$result_dir/waynav.out" \
		"$result_dir/waynav.log" \
		"$result_dir/sway.log" \
		"$result_dir/niri.log" \
		"$result_dir/niri-wayland-info.txt" \
		"$result_dir/missing-protocols"; do
		if [[ -e $file ]]; then
			printf '%s:\n' "$file" >&2
			cat -- "$file" >&2
		fi
	done
}
