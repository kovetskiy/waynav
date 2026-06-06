# @file int/setup.sh
# @brief Load shared integration test helpers.
# @description
#   Sources the helper libraries used by the Docker-backed waynav integration
#   testcases and validates the shared environment supplied by int/run_tests.

tests:involve lib/config.sh
tests:involve lib/docker.sh
tests:involve lib/assertions.sh

waynav:int:load-defaults
waynav:int:validate-config
waynav:int:require-command "$WAYNAV_INT_DOCKER"
