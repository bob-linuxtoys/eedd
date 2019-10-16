
HARDWARE
The slide4 card has four slide potentiometers attached to a ten
bit analog-to-digital converter.  The ADC values are reported to
the host 10 times per second.


RESOURCES
The interface to the slide4 lets you look at the most recent samples
or to watch for values that have changed.

positions : Values of all four slide pots in the range 0 to 1023
   The current positions of the slide pots as four space-separated
decimal numbers in the range of 0 to 1023.  A zero value indicates
that the slide is close to the Baseboard connector.  Use dpcat to
monitor the pots for movement and dpget to get the most recently
reported positions.


