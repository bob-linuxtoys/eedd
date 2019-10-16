RFOB
The rfob peripheral decodes commands from a keyfob RF
transmitter.  You can specify the number of bits to expect
and baudrate.  LEDs indicate valid packets as well as the
start of a packet.


HARDWARE
The RF receiver input is on the lowest numbered pin on the
connector (pin 2 or pin 10).  The next pin (4 or 10) has
the RSSI signal from the receiver.  RSSI is not used in
this design.
  The third pin drives a red LED which flashes during a
receipt of RF data.  The red LED may light when there is
a lot of RF noise at the antenna.  The fourth pin drives
a green LED with toggle with each valid command packet.
  RFOB works with both the RF315 card which has a built-in
antenna for 315 MHz as well as the RFOB card which lets
you install your own receiver.  Use the RFOB card if you
wish to use a 434 MHz transmitter and receiver.


RESOURCES
The peripheral interfaces for RFOB include the data from
the keyfob transmitter as well as the receiver configuration.

cmds : command codes from a keyfob transmitter as a single
hexadecimal number terminated by a newline.  The resource
works with dpcat.

config : number of bits to expect in each command and the
baudrate in bits per second.  The default values of 24 and
560 work for most 315MHz keyfob transmitters.  Please check
the specifications of your transmitter before changing these
values.
   Some 434 MHz transmitters have a 24 bit command sent at
1700 bits per second.


EXAMPLES
Configure the peripheral for 24 bits at 560 bps and start
listening for command packets.

   dpset rfob config 24 560
   dpcat rfob cmds
