# Sourced by run.sh, filmstrip.sh and play.sh: the one Docker image the host harness
# runs in, built once and reused. Before this file every run did `apt-get install`
# in a fresh ubuntu:20.04 - fifteen minutes of package downloads per test run, far
# longer than a prompt-cache window - and a run stopped from outside left its
# anonymous container behind.
#
# ensure_harness_image  - build $HARNESS_IMAGE if it is missing (seconds, once).
# harness_container NAME - name a container and remove it on exit/INT/TERM, so a
#                          stopped run never leaves a container running.
# No apostrophes in the inline Dockerfile: callers pass it through printf.
HARNESS_IMAGE="${HARNESS_IMAGE:-mister-harness:ubuntu20}"

ensure_harness_image() {
	docker image inspect "$HARNESS_IMAGE" >/dev/null 2>&1 && return 0
	echo "building $HARNESS_IMAGE (ubuntu:20.04 + gcc g++ libimlib2-dev curl, once)" >&2
	# curl + ca-certificates are for play.sh: the art fetcher forks curl.
	printf 'FROM ubuntu:20.04\nRUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends gcc g++ libc6-dev libimlib2-dev curl ca-certificates && rm -rf /var/lib/apt/lists/*\n' \
		| docker build -q -t "$HARNESS_IMAGE" - >/dev/null
}

harness_container() {
	HARNESS_CONTAINER="$1"
	# shellcheck disable=SC2064
	trap "docker rm -f '$HARNESS_CONTAINER' >/dev/null 2>&1 || true" EXIT INT TERM
}
