# OpenWebRX 1.2.2, the official image an operator gets from the wiki's
# Docker page and the Docker Hub page (`jketterl/openwebrx:stable`), pinned
# by the digest that tag resolved to on 2026-09-27 (a manifest list; its
# amd64 image is sha256:58686d24893664f7a72570ad8e1bf106f980dac4fccfe84af2b6e2486557e2ba).
# Nothing of OpenWebRX is rebuilt; only the lab's start hook is added.
FROM jketterl/openwebrx@sha256:d0a7e2efbce3d8828c902bde2e31ee28aeff1ad9ef67842ab0ab889e06c0e3d9

# Deviation: the lab runs every container with a read-only root and tmpfs
# on the paths receiver.json lists. The image's s6-overlay (v1) links its
# service directories into /var/run/s6 unless told the root is read-only,
# and would then try to write supervise state into /etc/services.d.
ENV S6_READ_ONLY_ROOT=1

# Deviation: the documented run keeps settings on a volume at
# /var/lib/openwebrx. The lab mounts the tier's settings.json read-only at
# /etc/fernbench and copies it into that directory (a tmpfs) before the
# image's own run.sh starts. If the harness does not pass an admin
# password, one is generated here for the documented OPENWEBRX_ADMIN_*
# variables and kept only in a 0600 file on that tmpfs, never in a log.
COPY --chmod=0755 <<'EOF' /opt/fernbench/start.sh
#!/bin/bash
set -euo pipefail
install -m 0644 /etc/fernbench/settings.json /var/lib/openwebrx/settings.json
if [[ -n "${OPENWEBRX_ADMIN_USER:-}" && -z "${OPENWEBRX_ADMIN_PASSWORD:-}" ]]; then
  OPENWEBRX_ADMIN_PASSWORD=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
  export OPENWEBRX_ADMIN_PASSWORD
  (umask 077 && printf '%s\n' "$OPENWEBRX_ADMIN_PASSWORD" > /var/lib/openwebrx/fernbench-admin-password)
fi
exec /opt/openwebrx/docker/scripts/run.sh "$@"
EOF

# The image runs S6_CMD_ARG0 as its main program; point it at the hook,
# which ends in the image's own /opt/openwebrx/docker/scripts/run.sh.
ENV S6_CMD_ARG0=/opt/fernbench/start.sh
