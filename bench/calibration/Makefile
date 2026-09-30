CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror
PULSE_CFLAGS ?= $(shell pkg-config --cflags libpulse)
PULSE_LIBS ?= $(shell pkg-config --libs libpulse)

all: build/sink_trace.so

build:
	mkdir -p $@

build/sink_trace.so: sink_trace.c | build
	$(CC) $(CFLAGS) -fPIC -shared $< -ldl -pthread -o $@

build/monitor: monitor.c | build
	$(CC) $(CFLAGS) $(PULSE_CFLAGS) $< $(PULSE_LIBS) -o $@

.PHONY: all
