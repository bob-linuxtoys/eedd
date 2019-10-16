============================================================

enumerator

At system startup the enumerator interrogates the FPGA to
get a list of the peripherals built into the FPGA image.
It then does a edloadso on the peripherals listed.
Other plug-in modules communicate with this plug-in
using this plug-in's 'tx_pkt()' routine.  Each plug-in
that manages an FPGA peripheral must offer a 'rx_pkt'
routine.


RESOURCES
port : The full path to the Linux serial port device.
Changing this causes the old device to be closed and
the new one opened.  The default value of 'device' is
/dev/ttyUSB0.

license : the text of the license for the FPGA image.

copyright : the text of the copyright of the FPGA image.


EXAMPLES
Use ttyS2 at 9600 baud.  Use GPIO pin 14 for interrupts
from the FPGA.  Start monitoring data from the FPGA and
send the command sequence b0 00 12 34 56.

 dpset enumerator port /dev/ttyUSB1
 dpget enumerator license
 dpget enumerator copyright


