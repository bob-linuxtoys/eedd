
HARDWARE
    This peripheral provides the interface to an ATmega88PB AVR.  
An AVR peripheral is typically used to perform immediate, OS
independent initialization activity.  The complete datasheet for 
the ATMEGA88PB device can be found at:
  http://ww1.microchip.com/downloads/en/DeviceDoc/40001909A.pdf


RESOURCES
    There are 2 separate categories of resources for this peripheral,
programming related and real time data memory access.  The programming 
related resources allow flashing of the program and EEPROM memories.
The programming jumper must be installed to use the programming
related resources.
    The data memory access resources allow you to read and write both
RAM and CPU registers while the program is running.  Your host
application would normally control the AVR application using these
resources.

program:
    Use this resource to load a program into the AVR or to read the
program that is currently loaded.  Be sure the programming jumper is
in place and note that the file path must be fully qualified.
Example:
    dpset avr program /home/me/myavr/led.hex
    dpget avr program /home/me/myavr/ledcheck.hex

eeprom:
    Use this resource to read and write the AVR's EEPROM.  To flash
the EEPROM, specify the beginning address in the EEPROM followed by
the space separate hex values to be flashed.  To dump the EEPROM,
specify the beginning address in the EEPROM followed by the number
of bytes to dump.  EEPROM addresses range from 0x00 to 0x1ff.  Be
sure the programming jumper is installed.  At most twelve bytes
can be written at one time.  The forms of the commands are:
    dpset avr eeprom <address> <byte1> <byte2> ...
    dpget avr eeprom <address> <count>
Example:
    dpset avr eeprom 01f2 45 67 89 ab  # write 4 bytes of EEPROM
    dpget avr eeprom 01f2 4            # read 4 bytes of EEPROM
    dpset avr eeprom 0000 77           # set default LED blink rate
 
vram:
    Use this resource to read and write RAM in the AVR.  Mapping
the virtual RAM addresses to physical addresses is done in the SPI
receive character interrupt handler.  The sample LED application
maps vram addresses into the 'hostRegs' array of bytes.  Commands
are of the form:
    dpset avr vram <address> <byte1> <byte2> ...
    dpget avr vram <address> <count>
Example:
    dpset avr vram 00 77     # set working LED blink rate
    dpget avr vram 01 01     # enable the LED flasher

fifo:
    Use this resource to write many values to a single RAM location, 
i.e. host register, in the AVR.  Note that currently there is way
to read many values from a single host register.  Also, any one
transaction is limited to 12 bytes of data written to the host
register.  The sample fifo application shows the usage of this 
resource.  Commands are of the form:
    dpset avr fifo <address> <byte1> <byte2> ...
Example:
    dpset avr fifo 0 5 6 7 8 9a bc de    # write 7 values to a fifo
                                         # through host register 0

reg:
    Use this resource to directly read and write the AVR hardware
registers.  Register addresses must be in the range 0x23-0xc6.
Please see the register summary in the data sheet for a list of
the hardware registers.  Addresses and data are given in hex and
commands are of the form:
    dpset avr reg <address> <byte1> <byte2> ...
    dpget avr reg <address> <count>
Example:
    dpset avr reg 2b 80    # turn on the LED directly
    dpget avr reg 2a 2     # returns LED port DDR and PORT values

