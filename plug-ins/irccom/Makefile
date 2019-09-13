#
#  Name: Makefile
#
#  Description: This is the Makefile for the hellodemo plugin
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
#

plugin_name = irccom

INC = ../include
LIB = ../../build/lib
OBJ = ../../build/obj

includes = $(INC)/eedd.h readme.h

# define target plug-in here
object = $(OBJ)/$(plugin_name).o
shared_object = $(LIB)/$(plugin_name).$(SO_EXT)

DEBUG_FLAGS = -g
RELEASE_FLAGS = -O3
CFLAGS = -I$(INC) $(DEBUG_FLAGS) -fPIC -c -Wall

all: $(shared_object)

$(LIB)/%.$(SO_EXT): %.o readme.h
	$(CC) $(DEBUG_FLAGS) -Wall $(SO_FLAGS),$@ -o $@ $<

readme.h: readme.txt
	echo "static char README[] = \"\\" > readme.h
	cat readme.txt | sed 's:$$:\\n\\:' >> readme.h
	echo "\";" >> readme.h

$(object) : $(includes)

clean :
	rm -rf $(shared_object) $(object) readme.h

install:
	/usr/bin/install -m 644 $(shared_object) $(INST_LIB_DIR)

uninstall:
	rm -f $(INST_LIB_DIR)/$(plugin_name).$(SO_EXT)

.PHONY : clean install uninstall

