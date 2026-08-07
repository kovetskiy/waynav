#!/usr/bin/env bash
# @file int/lib/start-headless-sway.sh
# @brief Start Sway with two wlroots headless outputs.
# @description
#   Container entrypoint used by the Sway integration testcase. It creates a
#   private Wayland runtime directory, writes a minimal Sway config, and execs
#   Sway on two headless outputs.

set -euo pipefail

readonly XDG_DIR=/tmp/xdg
readonly SWAY_CONFIG=/tmp/sway.conf

:write-config() {
	cat >"$SWAY_CONFIG" <<'EOF'
xwayland disable
output HEADLESS-1 resolution 1280x720 position 0 0
output HEADLESS-2 resolution 1024x768 position 1280 0
EOF
}

:main() {
	export XDG_RUNTIME_DIR=$XDG_DIR
	export WLR_BACKENDS=headless
	export WLR_HEADLESS_OUTPUTS=2
	export WLR_LIBINPUT_NO_DEVICES=1
	export WLR_RENDERER=pixman

	mkdir -p "$XDG_RUNTIME_DIR"
	chmod 700 "$XDG_RUNTIME_DIR"
	:write-config

	exec sway --unsupported-gpu -d -c "$SWAY_CONFIG"
}

:main "$@"
