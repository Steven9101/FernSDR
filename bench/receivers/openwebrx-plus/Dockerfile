# OpenWebRX+ 1.2.125, the image its Docker Hub page installs
# (`slechev/openwebrxplus`, built from the luarvique PPA's .deb packages),
# pinned by the digest the 1.2.125 tag (and `latest`) resolved to on
# 2026-09-27 (a manifest list; its amd64 image is
# sha256:4b3849ab2b8648335181d56f7742fbfe2857546caf508d57ac728b447fc1e617).
# Nothing of OpenWebRX+ is rebuilt; only the lab's start hook is added.
FROM slechev/openwebrxplus@sha256:896c69d6da35b0ecc7d82efa8ed5663f9f69000ad83fbecf0b038468a163c01d

# Deviation: the documented run keeps settings on a volume at
# /var/lib/openwebrx. The lab mounts the tier's settings.json read-only at
# /etc/fernbench, and this s6-rc oneshot copies it into that directory (a
# tmpfs) before the image's openwebrx service runs /run.sh, whose
# `cp -n` then leaves it alone. If the harness passes
# OPENWEBRX_ADMIN_USER without a password, the oneshot generates one into
# s6's container environment, where /run.sh (with-contenv) reads the
# documented OPENWEBRX_ADMIN_* variables, and keeps a copy only in a 0600
# file on the tmpfs, never in a log.
COPY --chmod=0755 <<'EOF' /opt/fernbench/config.sh
#!/bin/bash
set -euo pipefail
install -m 0644 /etc/fernbench/settings.json /var/lib/openwebrx/settings.json
env_dir=/run/s6/container_environment
if [[ -s $env_dir/OPENWEBRX_ADMIN_USER && ! -s $env_dir/OPENWEBRX_ADMIN_PASSWORD ]]; then
  password=$(head -c 24 /dev/urandom | base64 | tr -d '/+=')
  (umask 077 && printf '%s' "$password" > "$env_dir/OPENWEBRX_ADMIN_PASSWORD" &&
    printf '%s\n' "$password" > /var/lib/openwebrx/fernbench-admin-password)
fi
EOF
COPY <<'EOF' /etc/s6-overlay/s6-rc.d/fernbench-config/type
oneshot
EOF
COPY <<'EOF' /etc/s6-overlay/s6-rc.d/fernbench-config/up
/opt/fernbench/config.sh
EOF
RUN touch /etc/s6-overlay/s6-rc.d/user/contents.d/fernbench-config \
          /etc/s6-overlay/s6-rc.d/openwebrx/dependencies.d/fernbench-config
