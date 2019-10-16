============================================================

HARDWARE
    Each BaseBoard4 has three buttons (S1, S2 and S3) and
eight LEDs (LED0 to LED7).  The bb4io peripheral controls
these resources and is standard on all FPGA builds for the
BaseBoard4.



RESOURCES
leds : The value of the LEDs as an eight bit hexadecimal
number.  This resource is both readable and writable.

buttons : The value of the buttons as a number between 0
and 7.  This resource works with dpget and dpcat.  

