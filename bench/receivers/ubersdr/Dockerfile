# UberSDR for the benchmark lab: the project's own image, unchanged, pinned
# by digest. UberSDR's docker-compose-dockerhub.yml runs madpsy/ka9q_ubersdr
# (0.1.66 is the latest release on the day; the `testing` tag is newer but
# not a release). Its Dockerfile downloads `latest` release artifacts at
# build time, so a rebuild from source would not be this image; the digest
# is the pin. radiod comes from Dockerfile.radiod.
#
#   flock /tmp/fernbench-build.lock docker build \
#       -f bench/receivers/ubersdr/Dockerfile -t fernbench/ubersdr:0.1.66 bench
FROM madpsy/ka9q_ubersdr:0.1.66@sha256:206b727b5d7cfbfcc21419caee93d99c99aaa2cb8d63079655547e3f46aa29e7
