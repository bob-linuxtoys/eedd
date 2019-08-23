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
	make -C hellodemo all
	make -C gamepad all
	make -C irccom all
	make -C tts all
	make -C gps all
	make -C vl53 all

clean:
	make -C hellodemo clean
	make -C gamepad clean
	make -C irccom clean
	make -C tts clean
	make -C gps clean
	make -C vl53 clean

install:
	make INST_LIB_DIR=$(INST_LIB_DIR) -C hellodemo install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gamepad install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C irccom install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C tts install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gps install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C vl53 install

uninstall:
	make INST_LIB_DIR=$(INST_LIB_DIR) -C hellodemo uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gamepad uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C irccom uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C tts uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gps uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C vl53 uninstall

.PHONY : clean install uninstall

