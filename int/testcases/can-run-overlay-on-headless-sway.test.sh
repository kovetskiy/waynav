waynav:int:build-image sway
waynav:int:prepare-build sway
waynav:int:start-sway
waynav:int:assert-container-socket \
	"$WAYNAV_INT_SWAY_CONTAINER" /tmp/xdg/wayland-1

waynav:int:focus-sway-output HEADLESS-1
waynav:int:run-sway-smoke
waynav:int:assert-waynav-smoke "$(waynav:int:host-result-dir sway)"
tests:assert-re \
	"$(waynav:int:host-result-dir sway)/waynav.log" \
	'overlay created: 1280x720 on HEADLESS-1'

waynav:int:focus-sway-output HEADLESS-2
waynav:int:run-sway-smoke
waynav:int:assert-waynav-smoke "$(waynav:int:host-result-dir sway)"
tests:assert-re \
	"$(waynav:int:host-result-dir sway)/waynav.log" \
	'overlay created: 1024x768 on HEADLESS-2'
