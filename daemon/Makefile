# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Kleinia Authors
#
# schedmgrd / schedmgr — kleinia policy manager daemon (GLib/GDBus only).
#
# Targets:
#   all    build schedmgrd and schedmgr
#   check  build and run the g_test unit tests (test-core)
#   clean  remove build artifacts

CC        ?= gcc
PKGCONFIG ?= pkg-config
CFLAGS    ?= -O2 -g -Wall -Wextra
CFLAGS    += -std=gnu11 $(shell $(PKGCONFIG) --cflags gio-2.0 glib-2.0)
LDLIBS    := $(shell $(PKGCONFIG) --libs gio-2.0 glib-2.0)

DAEMON_SRC = schedmgrd.c schedmgr-core.c dbus-server.c backend-sim.c backend-system.c
HEADERS    = schedmgr-core.h

all: schedmgrd schedmgr

schedmgrd: $(DAEMON_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC) $(LDLIBS)

schedmgr: schedmgr.c schedmgr-core.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ schedmgr.c schedmgr-core.c $(LDLIBS)

test-core: test-core.c schedmgr-core.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ test-core.c schedmgr-core.c $(LDLIBS)

check: test-core
	./test-core

clean:
	rm -f schedmgrd schedmgr test-core

.PHONY: all check clean
