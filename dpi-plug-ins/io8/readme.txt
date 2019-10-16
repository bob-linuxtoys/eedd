HARDWARE
   The io8 card has eight dedicated inputs and eight dedicated
outputs.  The peripheral can be configured so that a change on
an input line automatically sends the input status up to the
host.


RESOURCES
output : The value on the output pins as a 2 digit hex value. 
    You can read and write this resource using dpget and dpset.

input : The value on the input pins.  This resource works with
    dpget and dpcat.  A read (dpget) requires a round trip to
    the FPGA and may take a few milliseconds.  You can monitor
    the input pins using a dpcat command.  Using dpcat makes
    sense only if one or more of the inputs are configured as 
    an interrupt-on-change pin.


interrupt : An 'interrupt-on-change' enable as a 2 digit hex
    value.  When an input pin has its corresponding interrupt
    bit set any change in value on that pin causes an update
    to be sent to the host.  This feature makes reading the
    input pins something that can be done using select().
    This resource works with dpget and dpset.


EXAMPLES
    To output 11000011 to the outputs, make all inputs
interrupt-on-change, and start a data stream of new input
values :
        dpset io8 output c3
        dpset io8 interrupt ff
        dpcat io8 inputs

