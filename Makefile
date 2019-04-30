#
#  Name: Makefile
# 
#  Description: This is the top level Makefile for the empty daemon
#
#  Copyright:   Copyright (C) 2019 by Demand Peripherals, Inc
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

# Define default command prefix and UI port
CPREFIX ?= ed
DEF_UIPORT ?= 8870


PREFIX ?= /usr/local
INST_BIN_DIR = $(PREFIX)/bin
INST_LIB_DIR = $(PREFIX)/lib/$(CPREFIX)-libs
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
	make CPREFIX=$(CPREFIX) DEF_UIPORT=$(DEF_UIPORT) -C plug-ins all
	make INST_LIB_DIR=$(INST_LIB_DIR) DEF_UIPORT=$(DEF_UIPORT) \
		CPREFIX=$(CPREFIX) -C daemon all

clean:
	make -C plug-ins clean
	make -C daemon clean
	rm -rf build

install:
	mkdir -p $(INST_LIB_DIR)
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) \
		CPREFIX=$(CPREFIX) DEF_UIPORT=$(DEF_UIPORT) -C plug-ins install
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) \
		CPREFIX=$(CPREFIX) DEF_UIPORT=$(DEF_UIPORT) -C daemon install

uninstall:
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) \
		CPREFIX=$(CPREFIX) DEF_UIPORT=$(DEF_UIPORT) -C plug-ins uninstall
	make INST_BIN_DIR=$(INST_BIN_DIR) INST_LIB_DIR=$(INST_LIB_DIR) \
		CPREFIX=$(CPREFIX) DEF_UIPORT=$(DEF_UIPORT) -C daemon uninstall
	rmdir $(INST_LIB_DIR)

.PHONY: clean install uninstall

