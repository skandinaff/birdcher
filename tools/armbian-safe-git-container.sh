#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Armbian's build container accesses a bind-mounted cache created by the host.
# Git otherwise rejects that cache as a "dubious ownership" repository.  This
# hook scopes the safety exception to the ephemeral build container only.

# This no-op advertises the extension to Armbian's pre-Docker extension scan.
function add_host_dependencies__birdcher_safe_git_cache() {
	:
}

function host_pre_docker_launch__birdcher_safe_git_cache() {
	DOCKER_EXTRA_ARGS+=("--env" "GIT_CONFIG_COUNT=1")
	DOCKER_EXTRA_ARGS+=("--env" "GIT_CONFIG_KEY_0=safe.directory")
	DOCKER_EXTRA_ARGS+=("--env" "GIT_CONFIG_VALUE_0=*")
}
