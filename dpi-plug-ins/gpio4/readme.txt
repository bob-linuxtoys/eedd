============================================================

HARDWARE
   The gpio4 card gives direct access to each of the four pins
in a Baseboard slot.   Each GPIO pin can be an input or an
output, and changes at an input pin can trigger the sending
of the pin status up to the host.  The highest numbered pin on
the connector is controlled by the LSB in the control and data
registers.


RESOURCES
pins : The value on the GPIO pins.  A write to this resource
sets an output pin, and a read from it returns the current
value on the pins.  A read requires a round trip over the
USB-serial link and may take a few milliseconds.  Data is
given as a hexadecimal number.  You can monitor the pins
using a dpcat command.  Using dpcat only makes sense if one
or more of the pins are configured as input and as a source
of interrupts.

direction : The direction of the four pins as hexadecimal
digit.  A set bit makes the pin an output and a cleared bit
makes it an input.  The power up default is for all pins to
be inputs.  This resource works with dpget and dpset.

interrupt : An 'interrupt' enable mask.  When a pin is an
input and the interrupt bit is set for that pin, then when
the logic level of the pin changes, the FPGA builds and
sends a packet with the new value of the pins.  This ability
is called 'interrupt' since it is asynchronous and not polled.
The interrupt resource works with dpget and dpset.


