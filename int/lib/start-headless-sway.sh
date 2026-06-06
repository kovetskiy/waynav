#!/usr/bin/env bash
# @file int/lib/start-headless-sway.sh
# @brief Start Sway with wlroots' headless backend.
# @description
#   Container entrypoint used by the Sway integration testcase. It creates a
#   private Wayland runtime directory, writes a minimal Sway config, and execs
#   Sway on a headless output.

set -euo pipefail

readonly XDG_DIR=/tmp/xdg
readonly SWAY_CONFIG=/tmp/sway.conf

:write-config() {
	cat >"$SWAY_CONFIG" <<'EOF'
xwayland disable
output HEADLESS-1 resolution 1280x720
EOF
}

:main() {
	export XDG_RUNTIME_DIR=$XDG_DIR
	export WLR_BACKENDS=headless
	export WLR_LIBINPUT_NO_DEVICES=1
	export WLR_RENDERER=pixman

	mkdir -p "$XDG_RUNTIME_DIR"
	chmod 700 "$XDG_RUNTIME_DIR"
	:write-config

	exec sway --unsupported-gpu -d -c "$SWAY_CONFIG"
}

:main "$@"
