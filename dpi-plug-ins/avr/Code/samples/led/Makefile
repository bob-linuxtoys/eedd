TARGET=led
DP_INCLUDE_FILES=../../include/dpavr.h

MCU=atmega88
CLOCK=8000000
CFLAGS=-g -Wall -DF_CPU=$(CLOCK) -mcall-prologues -mmcu=$(MCU) -Os
LDFLAGS=-Wl,-gc-sections -Wl,-relax
CC=avr-gcc
OBJECT_FILES=$(TARGET).o
INCLUDE_FILES=$(DP_INCLUDE_FILES)

all: $(TARGET).hex

clean:
	rm -f *.o *.hex *.obj *.hex

%.hex: %.obj
	avr-objcopy -R .eeprom -O ihex $< $@

%.obj: $(OBJECT_FILES) $(INCLUDE_FILES)
	$(CC) $(CFLAGS) $(OBJECT_FILES) $(LDFLAGS) -o $@

program: $(TARGET).hex
#	avrdude -F -p m88 -c usbtiny -U flash:w:$(TARGET).hex
	dpset avr program `pwd`/$(TARGET).hex

