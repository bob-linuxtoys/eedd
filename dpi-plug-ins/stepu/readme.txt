============================================================

HARDWARE
The unipolar stepper motor controller uses all four pins on a
BaseBoard connector to control the on/off state of four open-
drain drivers connected to the four coils of a unipolar stepper
motor.



RESOURCES
The interface to the unipolar stepper motor controller offers
direction, step rate, full/half steps, holding current, and
step count,

config : Stepper motor configuration include the mode of
operation, the direction, the step rate, and the holding
current as a percentage of full current.  The configuration
is space separated and has the following format:
       [mode] [direction] [rate] [hold current %]\\n
   Three modes of operation are available: off, full-step, and
half-step.  The mode is specified as one of the words 'off',
'full', or 'half'.  The first letter of each word can also be
used.  The off mode removes all power to the drivers.  The
power on default mode is off.
   The direction is given as 'forward' for a step sequence of
ABCD and 'reverse' for a sequence of DCBA.  The single letters
'f' and 'r' can also be used.  The default is forward.
   The motor step rate is given in Hertz.  The driver tries to
use a step-rate as close as possible to the one specified.  For
example, a requested step-rate of 511 Hertz might result in an
actual step rate of 500 Hertz.  The step rate has 8 bits of
accuracy, and a range from 4 Hertz to 1 MHz.
   The holding current is given as a percentage of full current
and is in the range 0 to 99 percent.  
   Sample configuration lines include the following:
       off  forward  400  10
       half  reverse  125  25
       f f 200 99

count : A 12 bit integer with the number of steps to be taken.
This is a read/write value.  Setting this to zero stops the motor
but leaves the coils energized with the specified holding current.
This value is decremented by one for each step (or half step)
taken.  The motor stops when the count reaches zero.  A read of
count show how many steps remain to be taken but be aware that
latency in reading the value from the board means that the actual
steps remaining are probably fewer that what is reported.

addcount : A 12 bit integer that is synchronously added to count.
This resource is used to get accurate movement where the number
of steps is larger than will fit in the 12 bit count register.
For example, to accurately step out 6000 steps, you would write
4000 to count, and then some time later write 2000 to addcount.
This write-only resource works only with dpset.


EXAMPLES
  Step 6000 half steps ramping up to a step rate of 200.  Start
with a step rate of 40, then 80, then 200.  Set the holding
current to 15 percent of full current.
    dpset stepb config half forward 40 15
    dpset stepb count 4000
    (sleep to let motor accelerate to 40 steps/sec)
    dpset stepb config half forward 80 15
    (sleep to let motor accelerate)
    dpset stepb config half forward 200 15
    (sleep or loop reading count until count < 2000)
    dpset stepb addstep 2000


