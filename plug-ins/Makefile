#
#  Name: Makefile
#
#  Description: This is the top level Makefile for the plug-ins
#
#  Copyright:   Copyright (C) 2019 by Demand Peripherals, Inc.
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

all:
	make -C eedd all
	make -C hellodemo all
	make -C gamepad all

clean:
	make -C eedd clean
	make -C hellodemo clean
	make -C gamepad clean

install:
	make INST_LIB_DIR=$(INST_LIB_DIR) -C eedd install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C hellodemo install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gamepad install

uninstall:
	make INST_LIB_DIR=$(INST_LIB_DIR) -C eedd uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C hellodemo uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gamepad uninstall

.PHONY : clean install uninstall

