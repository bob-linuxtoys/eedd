TOUCH4

The touch4 peripheral provides four capacitive touch inputs.
Inputs have separate theshold values expressed as a percentage
drop from the average value over the last minute.


HARDWARE
The TOUCH4 card has four RC oscillators based on a 74HC14 Schmitt
trigger inverter.  The capacitance of a finger touch alters the
frequency of the RC oscillator.  The driver detects this change
and report the change if the change exceeds the percentage the
user has specified.


RESOURCES
The quad touch sensor offers a configurable threshold, an output
of the switch status, and a debug output of the oscillator 
counts.

thresholds: The percent changes from the average for that input
to cause a transition from untouched to touched.  For example,
a threshold of 10 percent with an average value of 6250 would
not report a change until the count reached a value of 6250
minus 625, or 5625.  A threshold of zero turns off that input.
This resource is read-write.

touch : The status of the four capacitive touch sensors as a
single hex digit.  Sensor 1 is the LSB and sensor 4 is the MSB.
This resource work with dpcat only.

counts : A debug output of the separate frequencies of the RC
oscillators.  Use this to help determine the correct threshold
value for a given input.  This resource work with dpcat only.


EXAMPLES
Enable channels one to three, each with a threshold of 4
percent.  
   dpset touch4 threshold 4 4 4 0
   dpcat touch4 touch

A sample output from 'touch' might be:
    0
    1
    3
    1
    0


NOTES:
  The trigger count is a percentage of the counts over the
last minute or so.  If you hold your finger on an input it
will 'adjust' to the new input and will switch from touched
to untouched even if you continue to hold your finger on
the input.


