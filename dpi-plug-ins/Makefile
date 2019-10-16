#
#  Name: Makefile
#
#  Description: This is the top level Makefile for the DP daemon drivers
#
#  Copyright:   Copyright (C) 2010-2019 Demand Peripherals, Inc.
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
#               Please contact Demand Peripherals if you wish to use this code
#               in a non-GPLv2 compliant manner.
#

all:
	make -C enumerator all
	make -C bb4io all
	make -C irio all
	make -C gpio4 all
	make -C adc812 all
	make -C dc2 all
	make -C quad2 all
	make -C servo4 all
	make -C roten all
	make -C slide4 all
	make -C in4 all
	make -C in32 all
	make -C out4 all
	make -C out4l all
	make -C out32 all
	make -C lcd6 all
	make -C ping4 all
	make -C ei2c all
	make -C espi all
	make -C tif all
	make -C io8 all
	make -C us8 all
	make -C touch4 all
	make -C count4 all
	make -C ws2812 all
	make -C aamp all
	make -C rtc all
	make -C qpot all
	make -C stepb all
	make -C stepu all
	make -C rcrx all
	make -C rfob all
	make -C pwmout4 all
	make -C avr all
	make -C dac8 all

clean:
	make -C enumerator clean
	make -C bb4io clean
	make -C irio clean
	make -C gpio4 clean
	make -C adc812 clean
	make -C dc2 clean
	make -C quad2 clean
	make -C servo4 clean
	make -C roten clean
	make -C slide4 clean
	make -C in4 clean
	make -C in32 clean
	make -C out4 clean
	make -C out4l clean
	make -C out32 clean
	make -C lcd6 clean
	make -C ping4 clean
	make -C ei2c clean
	make -C espi clean
	make -C tif clean
	make -C io8 clean
	make -C us8 clean
	make -C touch4 clean
	make -C count4 clean
	make -C ws2812 clean
	make -C aamp clean
	make -C rtc clean
	make -C qpot clean
	make -C stepb clean
	make -C stepu clean
	make -C rcrx clean
	make -C rfob clean
	make -C pwmout4 clean
	make -C avr clean
	make -C dac8 clean

install:
	make INST_LIB_DIR=$(INST_LIB_DIR) -C enumerator install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C bb4io install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C irio install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gpio4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C adc812 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C dc2 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C quad2 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C servo4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C roten install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C slide4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C in4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C in32 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C out4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C out4l install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C out32 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C lcd6 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C ping4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C ei2c install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C espi install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C tif install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C io8 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C us8 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C touch4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C count4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C ws2812 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C aamp install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C rtc install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C qpot install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C stepb install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C stepu install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C rcrx install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C rfob install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C pwmout4 install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C avr install
	make INST_LIB_DIR=$(INST_LIB_DIR) -C dac8 install
	sudo ldconfig

uninstall:
	make INST_LIB_DIR=$(INST_LIB_DIR) -C enumerator uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C bb4io uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C irio uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C gpio4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C adc812 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C dc2 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C quad2 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C servo4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C roten uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C slide4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C in4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C in32 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C out4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C out4l uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C out32 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C lcd6 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C ping4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C ei2c uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C espi uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C tif uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C io8 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C us8 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C touch4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C count4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C ws2812 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C aamp uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C rtc uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C qpot uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C stepb uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C stepu uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C rcrx uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C rfob uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C pwmout4 uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C avr uninstall
	make INST_LIB_DIR=$(INST_LIB_DIR) -C dac8 uninstall
	sudo ldconfig

.PHONY : clean install uninstall
