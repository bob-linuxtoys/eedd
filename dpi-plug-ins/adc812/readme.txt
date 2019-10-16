============================================================

HARDWARE
The ADC812 card performs a 12 bit ADC on eight inputs at a rate
between 4 HZ and 100 Hz.  Two inputs can be combined to form a
differential input for the ADC.  Differential inputs perform a
13 bit conversion.   The ADC812 uses the Microchip MCP3304.
Please see the datasheet for specifications and part details.


RESOURCES
The resources for the ADC812 let you specify the sample rate
and whether or not an input is differential.  The ADC samples
are available at the 'samples' resource.

config : the sample period in milliseconds and which inputs are
differential.  The sample period is given in decimal and must
be between 10 and 256.  The differential inputs are given as an
8 bit hexadecimal number.
   If differential bit 0 is set then value 0 reports the signed
13-bit value of input 0 minus input 1.  If bit 1 is set then value
1 reports the value of input 1 minus input0.  This pattern is the
same for the other differential pairs, 2/3. 4/5, and 6/7.
   This is a read-write resource and works with dpset and dpget
but not dpcat.

samples : eight space-separated 12-bin ADC readings as hex values.
There is one line of output for each set of samples.  Single-ended
inputs give an unsigned 12 bit result and differential inputs give
a signed 12 bit result (since the difference can be above or below
the adjoining pin).  You can use select() on this resource after
giving the dpcat command.


EXAMPLES
    dpset adc812 config 100 55  # the 55 is in hex
    dpcat adc812 samples

