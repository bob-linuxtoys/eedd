#
#  Name: Makefile
# 
#  Description: This is the top level Makefile for the empty daemon
#
#  Copyright:   Copyright (C) 2015 by Bob Smith (bsmith@linuxtoys.org)
#               All rights reserved.
# 
#  License:     This program is free software; you can redistribute it and/or
#               modify it under the terms of the Version 2 of the GNU General
#               Public License as published by the Free Software Foundation.
#               GPL2.txt in the top level directory is a copy of this license.
#               This program is distributed in the hope that it will be useful,
#               but WITHOUT ANY WARRANTY; without even the implied warranty of
#               MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#               GNU General Public License for more details.
# 
#

PREFIX ?= /usr/local
INST_BIN_DIR = $(PREFIX)/bin
INST_LIB_DIR = $(PREFIX)/lib/eedd
UNAME_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
SO_FLAGS := -shared -Wl,-soname
SO_EXT := so
ifeq ($(UNAME_S), Darwin)
	SO_FLAGS := -shared -Wl,-undefined -Wl,dynamic_lookup -Wl,-install_name
	SO_EXT := dylib
endif
export SO_FLAGS
export SO_EXT

all:
	mkdir -p build/bin
	mkdir -p build/lib
	mkdir -p build/obj
	make -C plug-ins all
	make INST_LIB_DIR=$(INST_LIB_DIR) -C daemon all

clean:
	make -C plug-ins clean
	make -C daemon clean
	rm -rf build

install:
	mkdir -p $(INST_LIB_DIR)
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) -C plug-ins install
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) -C daemon install

uninstall:
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) -C plug-ins uninstall
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) -C daemon uninstall
	rmdir $(INST_LIB_DIR)

dist: clean
	tar cvzf eedd.tgz --exclude-vcs --exclude-backups ../eedd/plug-ins ../eedd/daemon ../eedd/Makefile ../eedd/Docs

.PHONY: clean install uninstall

