
HARDWARE
   The DAC8 card provides eight 8-bit digital to analog
channels.  The card uses the BH2226 from Rohm.  Output
is ratiometric relative to Vcc.  A jumper lets you use
3.3 volts, or 5 volts for Vcc.  The output current limit
is 1 milliamp.  Exceeding this value will destroy the
part.
   You can use the card's prototyping area to add
buffers or a more precise voltage regulator for Vcc.


RESOURCES
The interface to the DAC8 lets you set the output voltage
on any one of the eight channels.

value:
   The DAC and it output in the range of 0 to FF.  A
value of FF provides a nominal output of (Vcc * 255)/256.
The DAC is specified as a digit in the range of 1 to 8
and the value is specified as a hex value in the range
of 0 to ff.

Example:
    # set DAC output #1 to half scale
    dpset dac8 value 1 80
    # set DAC output #2 to full scale
    dpset dac8 value 2 ff


