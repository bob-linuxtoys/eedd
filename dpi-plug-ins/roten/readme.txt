HARDWARE
   The roten peripheral is an interface to a rotary encoder
with push button.  The encoder switches are on pins 4 and
6 of the daughter card, the button is on pin 2, and an LED
is on pin 8.
   The interface to roten include a read-only rotary motion
counter and button status, and a read-write LED value.


RESOURCES
encoder : count and button status
   A read-only resource (dpget/dpcat) with the number of
counts up or down since the last output and the current
state of the button.  The count is always within the
range of 63 to -64.  The count and button status are
given on the same line with a space separating the values.
For example five counts counterclockwise and no button
press would be reported as:
    -5 0


led : the state of the LED
   The state of the LED.  Use dpset with a value of '0'
to turn the LED off and a '1' to turn it on.  A dpget
returns the current state of the LED.


NOTES
   On occasion you may see an output from encoder 
something like this:
-1 0
0 0
0 0
-1 0

The '0 0' values can be ignored.  This occurs because
the hardware detected a change but the encoder went
back to its previous state before the FPGA could report
the new value.  You can think of it as switch bounce
which will have no impact on the usability of the rotary
encoder.

