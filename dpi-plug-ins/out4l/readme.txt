============================================================

HARDWARE
    Four FPGA pins configured as outputs.  


RESOURCES
outval : The value of the four output pins as a single
hexadecimal number.  This resource is both readable and
writable.  It works with dpget and dpset.  A one sets
the output high.  The default value is 0 in out4l and
is 0xf in out4.

