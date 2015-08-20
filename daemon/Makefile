#
#  Name: Makefile
#
#  Description: This is the Makefile for the empdy daemon
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


INC = ../plug-ins/include
BIN = ../build/bin
OBJ = ../build/obj

includes = $(INC)/main.h

objects = $(OBJ)/main.o $(OBJ)/util.o $(OBJ)/ui.o
edcliobjects  = $(OBJ)/edcli.o

DEBUG_FLAGS = -g -ggdb
RELEASE_FLAGS = -O3
CFLAGS = -I$(INC) $(DEBUG_FLAGS) -D LIB_DIR="\"$(INST_LIB_DIR)"/\" -Wall -pthread

all: eedd edcli

eedd : $(objects)
	$(CC) -o $(BIN)/$@ $(objects) -rdynamic -ldl

edcli : $(edcliobjects)
	$(CC) -o $(BIN)/$@ $(edcliobjects)

$(OBJ)/%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $^

install:
	/usr/bin/install -m 755  $(BIN)/eedd $(INST_BIN_DIR)
	/usr/bin/install -m 755  $(BIN)/edcli $(INST_BIN_DIR)
	ln -sf $(INST_BIN_DIR)/edcli $(INST_BIN_DIR)/edlist
	ln -sf $(INST_BIN_DIR)/edcli $(INST_BIN_DIR)/edset
	ln -sf $(INST_BIN_DIR)/edcli $(INST_BIN_DIR)/edget
	ln -sf $(INST_BIN_DIR)/edcli $(INST_BIN_DIR)/edcat
	ln -sf $(INST_BIN_DIR)/edcli $(INST_BIN_DIR)/edloadso

uninstall:
	rm -f $(INST_BIN_DIR)/eedd
	rm -f $(INST_BIN_DIR)/edcli
	rm -f $(INST_BIN_DIR)/edlist
	rm -f $(INST_BIN_DIR)/edset
	rm -f $(INST_BIN_DIR)/edget
	rm -f $(INST_BIN_DIR)/edcat
	rm -f $(INST_BIN_DIR)/edloadso


.PHONY : clean
clean :
	rm -f *.o

