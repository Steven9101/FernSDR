# SPDX-License-Identifier: GPL-3.0-or-later
#
# The lab's front end drivers for radiod, built against the headers of the
# radiod they will be loaded into (the struct layout differs between
# ka9q-radio and the madpsy fork, so a driver built for one must not be
# loaded into the other):
#
#   make KA9Q_SRC=/path/to/ka9q-radio/src [ARCHOPTS=-march=native]
#
# KA9Q_SRC must have been built first (radiod's Makefile generates
# config_paths.h, which misc.h includes). The flags are radiod's own
# release flags; ARCHOPTS should match what radiod was built with.
# -iquote, not -I: radiod's src/ has a sched.h of its own, which -I would
# put in place of the system's <sched.h> inside <pthread.h>.
KA9Q_SRC ?= ../ka9q-radio/src
CC ?= cc
ARCHOPTS ?=
CFLAGS ?= -O3 -DNDEBUG=1
ALL_CFLAGS = $(CFLAGS) $(ARCHOPTS) -std=gnu11 -pthread -fPIC -Wall -Wextra -D_GNU_SOURCE=1 \
	-funsafe-math-optimizations -fno-math-errno -freciprocal-math -fno-trapping-math \
	-iquote $(KA9Q_SRC)
LDLIBS = -lm -lpthread

DRIVERS = rtltcp.so fifos16.so

all: $(DRIVERS)

%.so: %.c
	$(CC) $(ALL_CFLAGS) -shared -o $@ $< $(LDLIBS)

clean:
	rm -f $(DRIVERS)

.PHONY: all clean
