waynav:int:build-image niri
waynav:int:prepare-build niri
waynav:int:run-niri-smoke
waynav:int:assert-niri-protocols "$(waynav:int:host-result-dir niri)"
waynav:int:assert-waynav-smoke "$(waynav:int:host-result-dir niri)"
waynav:int:assert-no-protocol-errors \
	"$(waynav:int:host-result-dir niri)/niri.log"
