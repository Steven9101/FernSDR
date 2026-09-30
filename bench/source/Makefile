# The lab's own programs. They run on the host, not in the release, so
# they may use the host's C library freely.
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wshadow -pthread
BUILD := build

all: $(BUILD)/pace

$(BUILD)/pace: pace.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< -lm

$(BUILD):
	mkdir -p $@

test: all
	python3 test_scene.py
	python3 test_pace.py

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
