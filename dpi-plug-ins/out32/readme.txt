============================================================

HARDWARE
    The out32 card has thirty-two pins that are latched 
using four 74XX395 octal latches.  Outputs are at 3.3
volts.


RESOURCES
outval : The value of the output pins as a single 32 bit
hexadecimal number.  This resource is both readable and
writable.  It works with dpget and dpset.  A set bit
sets the corresponding output high.

