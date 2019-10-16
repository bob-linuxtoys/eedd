RCRX
The rcrc peripheral decodes a stream of radio control pulses into
timing values.  You can specify how many channels are in the data
stream.  The update rate depends on the data stream and is usually
once every 20 milliseconds.


HARDWARE
The RC Decoder input from the RC radio is on the lowest numbered
pin on the connector (pin 2 or pin 10).  The next pin (4 or 10)
is driven high to indicate reception of valid RC data.  This pin
is normally tied to an LED through a 180 Ohm resistor.  The
remaining two pins on the connector are general purpose I/O pins.

If you have a receiver with multiple outputs you may want to
combine the individual channels into one stream using diodes in
a wired-OR configuration with a pull-down resistor.  See the
web page for this peripheral to see a sample wiring diagram.


RESOURCES
The device interfaces for the RC Decoder include the number of
RC channels to expect, the output of the RC decoder, and the
direction and current value of the GPIO pins.

nchan : the number of RC channels in the received signal as a
single ASCII digit in the range of two to eight and terminated
by a newline.  The system uses the number of expected channels
in the data stream to help check the validity of the stream
data.  This resource works with dpset and dpget.

rcdata : the decoded RC signal as string of space separated
channel times given in nanoseconds followed by the value of
input that started the RC frame.  Times are in hundreds of
nanoseconds.  That is a value of 10455 represents an interval
of 1045500 ns or 1.0455 milliseconds.  Normally a new frame is
available every 20 milliseconds.

A frame can start any time after a 3 millisecond period without
any transitions.  A new frame can start with either a transition
from low to high or from high to low.  


gpiodir : direction of the two GPIO pins as a single numeric digit
followed by a newline.  A value of 0 make both lines an input and
a value of 3 makes both lines an output.  A value of 2 make the
lower numbered GPIO pin (pin 6 or pin 14) an output and a value of
1 makes the higher numbers pin (8 or 16) an output.  This resource
works with dpset and dpget.

gpioval : value on the GPIO pins.  Write to this resource to set
an output and read from it to get the current value on input pins.
The LSB of the return value is the value on the highest numbered
pin (pin 8 or pin 16).  This resource works with dpset and dpget.


EXAMPLES
Make both auxiliary pins output and set them low.  Configure the
peripheral to expect a stream with 6 channels of data and start
reading the stream of RC samples.

   dpset rcrx gpiodir 3
   dpset rcrx gpioval 0
   dpset rcrx nchan 6
   dpcat rcrx rcdata

A sample output from rcdata might be:
 4923  9303  5065 12165  4869 10471  4838 10871  4744 10447  4761 13445 1
 5074  9282  4873 12306  4873 10513  5085 10586  4784 10362  4874 13378 1
 4856  9390  4946 12121  5024 10465  4831 10823  4828 10407  4744 13472 1
 4967  9327  4926 12295  4896 10415  5039 10667  4790 10387  4761 13515 1
 4926  9334  4924 12117  5025 10457  4752 10942  4808 10285  4846 13488 1

It is up to you perform a sanity check on the values received.  For
example, the following might be the output as the receiver drifts
out of range of the transmitter:

 4278 10785  4421  2663  7945  5225  9685  4929 11572  4906 10284  4921 1
 6766  8420  4940 10338  4546 10496  4977 10924  4160 10977  4368  7228 1
13811  4967 10466  4486 10421  5086 10427  4275 11200  4498  4068  6427 0
 4477  9985  5357 10510  5002 10453  4672 10680  4698 10751  4264 10825 1
22081  4886 10322  5009 10237  5365  9934  4267 10595  5385 10511  4843 0
 4280 10800  4354  4451  6299  5030 10133  4931 10657  4942 10385  5851 1
 4501  3118  7364  5199 10176  4690 10428  5040 10334  4359 11301  4217 1
12782  4980 10423  3973 10937  5104 10611  3837 11483  4269  5269  5611 0
21117  4995 10137  5153 10224  5524  9807  3784 11587  5136 10222  4930 0


