============================================================

HARDWARE
The dual quadrature decoder provides two independent channels of
quadrature decoding.  The first channel appears on the Baseboard
connector pins 2 and 4, or on pins 10 and 12 depending on the slot
number for the peripheral.  The second channel is on pins 6 and 8,
or on pins 14 and 16.  A positive step indicates that the lower
numbered input pin (2, 6, 10, 14) went high before the higher
numbered pin (4, 8, 12, 16).  That is, the lowered numbered pin
is leading the quadrature cycle.  The maximum input frequency for
the decoder is one Megahertz.

RESOURCES
The dual quadrature decoder offers a  configurable update rate
and a select()'able interface to the latest readings.

counts : The quadrature step counts and the number of seconds it
took to accumulate those counts.  The first number is a signed
integer that is the step count and (by the sign) the direction
of the first quadrature input.  The second number is a float that
is the number of seconds it took to accumulate the step count.
The third and fourth numbers are the count and accumulation time
for the second input channel.
  This resource is read-only.  You can use the dpcat command and
select() to get continuous updates into your program.

update_period : Update period for the counts resource in milliseconds.
That is, the dpcat command and select() on counts will will give a
readable file descriptor every update_period milliseconds.  Setting
this to zero turns off all output from the quadrature decoder.
The update period must be between 0 and 60 milliseconds in steps
of 10 milliseconds.   That is, valid values are 0, 10, 20, 30, 40,
50, or 60 milliseconds.


EXAMPLES
Imagine that the sample rate is 10 milliseconds and that the
channel 0 input step rate is about 160 hertz.  A sample output
from 'counts' might be:
    1   0.007472   0   0.000000
    0   0.000000   0   0.000000
    1   0.007476   0   0.000000
    1   0.007474   0   0.000000
    0   0.000000   0   0.000000

You can not get an accurate frequency from the count alone but by
including the accumulation time you can see that the input frequency
is about 133.8 Hertz.  The accumulation time is measured to the
microsecond.




