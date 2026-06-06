waynav:int:build-image sway
waynav:int:prepare-build sway
waynav:int:start-sway
waynav:int:assert-container-socket \
	"$WAYNAV_INT_SWAY_CONTAINER" /tmp/xdg/wayland-1
waynav:int:run-sway-smoke
waynav:int:assert-waynav-smoke "$(waynav:int:host-result-dir sway)"
