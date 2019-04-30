#
#  Name: Makefile
#
#  Description: This is the Makefile for the empdy daemon
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


INC = ../plug-ins/include
BIN = ../build/bin
OBJ = ../build/obj

includes = $(INC)/main.h

objects = $(OBJ)/main.o $(OBJ)/util.o $(OBJ)/ui.o
edcliobjects  = $(OBJ)/cli.o

DEBUG_FLAGS = -g -ggdb
RELEASE_FLAGS = -O3
CFLAGS = -I$(INC) $(DEBUG_FLAGS) -D LIB_DIR="\"$(INST_LIB_DIR)"/\" -Wall -pthread
CFLAGS += -D CPREFIX="\"$(CPREFIX)"\" -D DEF_UIPORT=$(DEF_UIPORT)

all: $(CPREFIX)daemon $(CPREFIX)cli

$(CPREFIX)daemon : $(objects)
	$(CC) -o $(BIN)/$@ $(objects) -rdynamic -ldl

$(CPREFIX)cli : $(edcliobjects)
	$(CC) -o $(BIN)/$@ $(edcliobjects)

$(OBJ)/%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $^

install:
	/usr/bin/install -m 755  $(BIN)/$(CPREFIX)daemon $(INST_BIN_DIR)
	/usr/bin/install -m 755  $(BIN)/$(CPREFIX)cli $(INST_BIN_DIR)
	ln -sf $(INST_BIN_DIR)/$(CPREFIX)cli $(INST_BIN_DIR)/$(CPREFIX)list
	ln -sf $(INST_BIN_DIR)/$(CPREFIX)cli $(INST_BIN_DIR)/$(CPREFIX)set
	ln -sf $(INST_BIN_DIR)/$(CPREFIX)cli $(INST_BIN_DIR)/$(CPREFIX)get
	ln -sf $(INST_BIN_DIR)/$(CPREFIX)cli $(INST_BIN_DIR)/$(CPREFIX)cat
	ln -sf $(INST_BIN_DIR)/$(CPREFIX)cli $(INST_BIN_DIR)/$(CPREFIX)loadso

uninstall:
	rm -f $(INST_BIN_DIR)/$(CPREFIX)daemon
	rm -f $(INST_BIN_DIR)/$(CPREFIX)cli
	rm -f $(INST_BIN_DIR)/$(CPREFIX)list
	rm -f $(INST_BIN_DIR)/$(CPREFIX)set
	rm -f $(INST_BIN_DIR)/$(CPREFIX)get
	rm -f $(INST_BIN_DIR)/$(CPREFIX)cat
	rm -f $(INST_BIN_DIR)/$(CPREFIX)loadso


.PHONY : clean
clean :
	rm -f *.o

