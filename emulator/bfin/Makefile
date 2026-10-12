# SPDX-License-Identifier: GPL-2.0-or-later
# cdj-gui-run, the GUI board on the vendored Blackfin core (see
# THIRD_PARTY.md).  scripts/build-cdj-gui-run.sh drives this.
#
#   make -C emulator/bfin O=build/cdj-gui-run
O      ?= ../../build/cdj-gui-run
CC     ?= cc
CFLAGS ?= -O3 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS += -std=gnu11 -I. -pthread

# MinGW: the link to MAIN is Winsock, and a static binary needs no MSYS2 DLL
# beside it (libwinpthread-1.dll): the launchers start it without MSYS2 on PATH.
ifneq (,$(findstring mingw,$(shell $(CC) -dumpmachine)))
EXE     = .exe
LDFLAGS += -static
LDLIBS  += -lws2_32
endif

SRCS = cdj_gui_run.c cdj_link.c bfin_core.c bfin_exec.c bfin_dsp.c bf531.c cdj_parent_watch.c
OBJS = $(patsubst %.c,$(O)/%.o,$(SRCS))
HDRS = $(wildcard *.h)

all: $(O)/cdj-gui-run$(EXE)

$(O):
	mkdir -p $(O)

$(O)/%.o: %.c $(HDRS) | $(O)
	$(CC) $(CFLAGS) -c $< -o $@

$(O)/cdj-gui-run$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(O)

.PHONY: all clean
