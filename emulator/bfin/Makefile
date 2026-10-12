# SPDX-License-Identifier: GPL-2.0-or-later
# cdj-gui-run, the GUI board on the vendored Blackfin core (see
# THIRD_PARTY.md).  scripts/build-cdj-gui-run.sh drives this.
#
#   make -C emulator/bfin O=build/cdj-gui-run
O      ?= ../../build/cdj-gui-run
CC     ?= cc
CFLAGS ?= -O3 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -std=gnu11 -I. -pthread

SRCS = cdj_gui_run.c cdj_link.c bfin_core.c bfin_exec.c bfin_dsp.c bf531.c cdj_parent_watch.c
OBJS = $(patsubst %.c,$(O)/%.o,$(SRCS))
HDRS = $(wildcard *.h)

all: $(O)/cdj-gui-run

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/cdj-gui-run: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -rf $(O)

.PHONY: all clean
