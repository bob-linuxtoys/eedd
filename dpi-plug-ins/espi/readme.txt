
HARDWARE
   This peripheral provides an encoded clock SPI interface.
The problem with normal SPI is that ringing on the SCLK line
can cause false clock edges.  This makes normal SPI unreliable
for distances of more than a few inches.  Encoded clock SPI
overcomes this problem and lets you use even tens of feet of
ribbon cable to connect an ESPI peripheral to the BaseBoard.
   ESPI works by combining three output lines from the FPGA
with a dual, edge-triggered D flip-flop.  Ringing at the D
input does not affect the Q output as long as the clock input
does not change.  Once a clock pulse is given, ringing at the
clock input does not change the Q output as long a the D input
does not change.  This is how ESPI works.  We do quadrature
encoding of the clock, data, and chip select lines and use a
dual D flip-flop to decode the signals.  Please see the protocol
reference for a schematic of the ESPI decoding circuit.

   Encoded clock SPI supports SPI Mode 0 and SPI Mode 2.  It
does not support SPI modes 1 or 3.  It can transfer as many as
14 bytes with one packet, and it supports four different clock
frequencies.  You can configure the chip select line to be
forced high, force low, active high during transfers or active
low during transfers.


RESOURCES
The device interfaces to the clock encoded SPI peripheral
let you specify SCLK clock frequency, the behavior of the
chip select line, and the data to and from the device.

config:
   The configuration for the ESPI port is entered into and
available from the config resource.  The configuration is
entered as two strings separated by space and terminated
by a new line.
   The meaning of the strings is given below.

CLOCK FREQUENCY
   The frequency of the sck signal in Hertz - the driver
will round the input frequency down to the next available
clock frequency.  Available clock frequencies are:
   2000000 -- 2 MHz
   1000000 -- 1 MHz
    500000 -- 500 KHz
    100000 -- 100 KHz

CHIP SELECT MODE
   Specifies the behavior of the chip select line.  The
following are the possible choices:
  'al'  -- active low: low during transfers, high otherwise
  'ah'  -- active high: high during transfers, low otherwise
  'fl'  -- forced low
  'fh'  -- forced high

A sample configuration line for a SPI port using SPI Mode 0
at 1 MHz, and with an active low chip select could be entered
into the device node as follows:
  dpset espi config 1000000 al
  
data:
    Due to the nature of SPI, dpget provides both read and write
functionality. Each read requires a write first, and bytes must
be provided to fill with resulting data.
    The data must be a single line of up to 14 space-separated (or space) 
hexadecimal numbers, corresponding to one packet of data. 
    Returns the data read from the SPI peripheral. The data consists of
a single line of space separated hexadecimal numbers which are
the data returned from the data written to the mosi interface.

Example:
    dpget espi data 03 00 00 00

Returns:
    00 00 00 10
Where '02' is the read command, and the '10' is the response read from
this example peripheral.

