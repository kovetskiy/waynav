# @file int/teardown.sh
# @brief Clean up integration test containers.
# @description
#   Removes Docker containers created by the integration testcases unless the
#   caller requested --keep-container.

tests:involve lib/config.sh
tests:involve lib/docker.sh

waynav:int:load-defaults
waynav:int:cleanup-container "$WAYNAV_INT_SWAY_CONTAINER"
waynav:int:cleanup-container "$WAYNAV_INT_NIRI_CONTAINER"
