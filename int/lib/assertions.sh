# @file int/lib/assertions.sh
# @brief Assertions for waynav integration test results.
# @description
#   Provides harness-backed assertions for waynav smoke logs and nested niri
#   protocol advertisements.

# @description Assert that waynav exited cleanly and produced expected logs.
# @arg $1 string Host result directory containing waynav.out and waynav.log.
# @stdout None.
# @stderr Harness diagnostics on assertion failure.
# @exitcode 0 If all assertions pass.
# @exitcode 88 If a harness assertion fails.
waynav:int:assert-waynav-smoke() {
	local result_dir=$1
	local out_file="$result_dir/waynav.out"
	local log_file="$result_dir/waynav.log"

	tests:assert-test -f "$out_file"
	tests:assert-test -f "$log_file"
	tests:assert-empty "$out_file"
	tests:assert-re "$log_file" 'loaded [0-9]+ bindings from '
	tests:assert-re "$log_file" 'overlay created:'
	tests:assert-re "$log_file" 'keyboard keymap loaded'
	tests:assert-re "$log_file" 'key: sym=0x3b mods=0x0'
	tests:assert-re "$log_file" 'exec: end'
	tests:assert-re "$log_file" 'exiting'
	waynav:int:assert-no-protocol-errors "$log_file"
}

# @description Assert that nested niri advertised all protocols waynav needs.
# @arg $1 string Host result directory containing niri smoke output files.
# @stdout None.
# @stderr Harness diagnostics on assertion failure.
# @exitcode 0 If all protocol assertions pass.
# @exitcode 88 If a harness assertion fails.
waynav:int:assert-niri-protocols() {
	local result_dir=$1
	local info_file="$result_dir/niri-wayland-info.txt"
	local socket_file="$result_dir/niri-socket"
	local proto=''

	tests:assert-test -f "$socket_file"
	tests:assert-re "$socket_file" '^wayland-[0-9]+'
	tests:assert-test -f "$info_file"

	for proto in \
		zwlr_layer_shell_v1 \
		zwlr_virtual_pointer_manager_v1 \
		wp_fractional_scale_manager_v1 \
		wp_viewporter \
		zxdg_output_manager_v1 \
		zwp_virtual_keyboard_manager_v1; do
		tests:assert-re "$info_file" "$proto"
	done
}

# @description Assert that files do not contain Wayland protocol errors.
# @arg $@ string Files to inspect.
# @stdout None.
# @stderr Harness diagnostics on assertion failure.
# @exitcode 0 If no protocol errors are found.
# @exitcode 88 If a harness assertion fails.
waynav:int:assert-no-protocol-errors() {
	local file=''

	for file in "$@"; do
		tests:assert-test -f "$file"
		tests:eval grep -Eiq \
			'invalid method|interface version|protocol error' "$file"
		tests:assert-fail
	done
}
