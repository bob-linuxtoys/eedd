HARDWARE
   The in32 card has thirty-two digital inputs.  Inputs are
nominally 3.3 volts but are 5 volt tolerant.  The peripheral
can be configured so that a change on an input line
automatically sends the input status up to the host.


RESOURCES
input : The value on the input pins.  This resource works with
    dpget and dpcat.  A read (dpget) requires a round trip to
    the FPGA and may take a few milliseconds.  You can monitor
    the input pins using a dpcat command.  Using dpcat makes
    sense only if one or more of the inputs are configured as 
    an interrupt-on-change pin.


interrupt : An 'interrupt-on-change' enable as a 8 digit hex
    value.  When an input pin has its corresponding interrupt
    bit set any change in value on that pin causes an update
    to be sent to the host.  This feature makes reading the
    input pins something that can be done using select().
    This resource works with dpget and dpset.


EXAMPLES
    Make the low 16 inputs interrupt-on-change, and start a
data stream of new input values :
        dpset in32 interrupt 0000ffff
        dpcat in32 inputs

