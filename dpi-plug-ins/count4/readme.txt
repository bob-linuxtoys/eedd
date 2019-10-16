COUNT4

The count4 peripheral provides four independent channels of count
and period information.  The sampling interval can be 0, 10, 20,
or 50 milliseconds, and the counters can count rising, falling,
or both edges in a pulse stream.
   Each count reading is followed by the number of seconds it
took to reach that count.  The measured count interval is accurate
to the microsecond.  Having a count and an interval lets you
computer the period with high accuracy even at very low count
rates.


HARDWARE
The first channel appears on the BaseBoard connector pin 2 or on
pin 10 depending on the slot number for the peripheral.  The
second channel is on pin 6 or on pin 14.
   The count4 peripheral is most often used with the GPIO4 card
or the GPIO4-ST card, both of which can limit the input to the
maximum of 3.3 volts as required by the FPGA.
   The maximum input frequency for the counter is 1.00 Megahertz.


RESOURCES
The quad counter offers a configurable update rate and a choice
of counting on the leading edge, the trailing edge, or on both
edges. The output is a set of eight numbers that are four pairs
of count and interval information.  The count output works with
select().

update_rate : Update period for the counts resource in milliseconds.
That is, the dpcat command and select() on counts will will give a
readable file descriptor every update_rate milliseconds.  This
update period must be between 10 and 60 milliseconds in steps
of 10 milliseconds.   That is, valid values are 10, 20, 30, 40,
50, or 60 milliseconds.

edges:  Which edges to count as four single digit numbers in the
range of 0 to 3.  A setting of 0 disables the counter, a setting
of 1 counts rising edged, 2 counts falling edges, and a setting
of 3 counts both edges.  Set all edges to 0 to turn off all 
output.

counts : Counts and intervals as four pairs of numbers where the
first number is an unsigned integer and the second number is the
number of seconds in the interval as a floating point number with
6 digits after the decimal point.  All values are separated by
spaces and each reading is terminated by a newline.
   This resource is read-only.  You can use the dpcat command
and select() to get continuous updates into your program.


EXAMPLES
Count both edges on channel one, disable channels two through
four, and set the sample rate to 50 milliseconds.

   dpset count4 edges 3 0 0 0
   dpset count4 update_rate 50
   dpcat count4 counts

A sample output from 'counts' might be:
    1   0.007472   0   0.000000   0   0.000000   0   0.000000
    0   0.000000   0   0.000000   0   0.000000   0   0.000000
    1   0.007476   0   0.000000   0   0.000000   0   0.000000
    1   0.007474   0   0.000000   0   0.000000   0   0.000000
    0   0.000000   0   0.000000   0   0.000000   0   0.000000

You can not get an accurate frequency from counts alone but by
including the accumulation time you can see that the input frequency
is about 133.8 Hertz.  The accumulation time is measured to the
microsecond,  You can use this to determine the accuracy of your
frequency estimate.





