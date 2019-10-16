============================================================

This directory contains the device entries for the ei2c card.

OVERVIEW
   In conjunction with the BaseBoard the ei2c card provides a
general purpose I2C interface to a Linux or Windows PC.  The
board supports clock rates of both 400 and 100 KHz.  The card
supports clock-stretching but the card should be the only
master on the bus.
   Reads and writes to the bus are organized as packets that
can have up to 13 data bytes.  You can intermix reads and writes
but each transition from read to write (or back) takes one byte
from the 14 data bytes available.
   Read responses are available on a separate node and each
response indicates success or failure of the packet.


HARDWARE
   The ei2c card has a prototyping area as well as connectors
for SCL, SDA, +3.3, +5, and ground.  Jumpers let you select
the pull-up resistor voltage and also let you use external
pull-ups if you wish.


RESOURCES
   The device interfaces for the ei2c card include clock rate
configuration, a node to send packets, and a node to receive
packet responses.

config:
   The clock rate in Kilohertz as a single integer followed
by a newline.  Valid clock rates are 400 and 100.

data:
   A read-only resource that accepts the I2C packet.  Data
bytes to be written are given in hexadecimal, and an 'R' marks
bytes to be read from slave devices on the bus.  The command
format for a packet is:
	<addr> <hex|R> ... <hex|R>

Due to the nature of I2C, dpget provides both read and write
functionality. Each dpget writes an I2C packet to the target
and gets the response.


EXAMPLE
   The Microchip MCP23008 is an 8 bit expansion port with an
I2C interface.  The part has ten internal read-write registers
for configuration and data.  Commands to the part have the
I2C address, a write of the register number, and a read or
write of the data for that register.  The command to write
a 55 to register 3 in the MCP23008 with an address of 0x20
would be:
	dpget ei2c data 20 03 55
The response to this command would be:
        A 20 w 03 55

A read of that same register would be:
	dpget ei2c data 20 03 R
The response to this command would appear as:
        A 20 w 03 r 55


