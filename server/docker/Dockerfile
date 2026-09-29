# FernSDR as a container image, built from a release's tree (make -C server
# docker). The program is static; Alpine brings what it runs beside it: curl
# for the module catalogue, su-exec to leave root, tini to reap what modules
# leave behind. See docs/DEPLOYMENT.md, "In a container".
FROM alpine:3.22
RUN apk add --no-cache ca-certificates curl su-exec tini tzdata \
    && addgroup -S fernsdr && adduser -S -D -H -h /var/lib/fernsdr -s /sbin/nologin -G fernsdr fernsdr \
    && install -d -o fernsdr -g fernsdr -m 0700 /var/lib/fernsdr
COPY tree/ /opt/fernsdr/
COPY entrypoint.sh /usr/local/bin/fernsdr-entrypoint
ENV FERNSDR_CONTAINER=1
VOLUME /var/lib/fernsdr
WORKDIR /var/lib/fernsdr
EXPOSE 8073
ENTRYPOINT ["/sbin/tini", "--", "/usr/local/bin/fernsdr-entrypoint"]
