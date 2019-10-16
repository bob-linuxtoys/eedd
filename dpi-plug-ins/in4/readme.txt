
HARDWARE
   The Demand Peripherals in4 peripheral monitors four digital
inputs.  You can read the status of the inputs at any time or
you can select a set of inputs to monitor and block on a read
while waiting for one of the monitored inputs to change.  The
highest numbered pin corresponds to the MSB and the lowest
numbered pin is the LSB.


RESOURCES
   Resources for the in4 peripheral include a read-only data
register and a read-write configuration register to determine
which pins are monitored for changes.

inputs : The value on the 4 pins
    A dpget on this resource returns the current values of all
4 pins.  A dpget requires a round trip over the USB-serial link
and may take a few milliseconds.  Pin values are given as a
hexadecimal number with the value of the lowest numbered pin
in the LSB.
   When used with dpcat this resource reports the new value
of the pins after one of the monitored pins changes value.
Use this resource to wait for changes on the monitored pins.

interrupt : Which pins to monitor
   A change on a monitored pin causes a packet to be sent to
the host.  The packet contains the new values of all 4 pins
and is made available as a line of output at the 'inputs'
resource


EXAMPLES
   To monitor all four pins for changes use the following:
      dpset in4 config f
      dpcat in4 inputs
