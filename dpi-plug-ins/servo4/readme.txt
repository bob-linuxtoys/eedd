============================================================

HARDWARE
   The Quad Servo Controller provides four outputs that can
each control one PWM servo motor.  The servo pulse widths are
accurate to 50 nanoseconds.  The cycle time on each of the
four signals is 20 milliseconds.
   The servos are numbered from 0 to 3 with servo 0 on the
lowest numbered pin on the connector (pin 2 or pin 10).  Servo
3 is on the highest numbered pin (pin 8 or pin 16).  The servo
signals are not inverted and the power-on default values are
all high.


RESOURCES
   The device interfaces to the Quad Servo Controller include
individual controls for each servo as well as a control which
manipulates the servos as a group.

servo1, servo2, servo3, servo4 : Pulse width in nanoseconds.
Normal servo motors expect a pulse width in the range of
1000000 to 2000000 nanoseconds.  A 50 nanoseconds resolution
gives 20000 different valid values or about 15 bits of
accuracy.  This resource works with dpset and dpget.

servogroup: All four pulse widths in nanoseconds.
The servogroup resource is a convenient way to move all of
the servos at one time.  The syntax is a space separated
list of integer pulse widths followed by a newline.  A pulse
width of zero sets that output high and pulse width of -1
takes that pulse width out of the group move.
Some examples may help:
   # Move all pulse widths to center
   dpset servo4 servogroup 1500000, 1500000, 1500000, 1500000
   # Move servo2 counterclockwise
   dpset servo4 servo2 1000000
   # Move servos 1 and 3 but not servos 2 and 4
   dpset servo4 servogroup 1400000 -1 1600000 -1

If controlling the servos from the Bash command line be aware
that a value of '-1' may be seen as a command line option.
Use the '--' option to tell Bash that there are no more command
line options on the line:
   # In Bash move servos 1 and 3 but not servos 2 and 4
   dpset -- servo4 servogroup 1400000 -1 1600000 -1

   A read of servogroup returns the current positions of all
servos as a space separate list of pulse widths measured in
nanoseconds.  This resource works with dpset and dpget.

