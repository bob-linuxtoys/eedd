============================================================

gamepad Plug-in
The gamepad plug-in provides a simple ASCII interface
to Linux joystick or gamepad devices.  The plug-in has
user configurable resources for the Linux device name
and the period for status writes to the 'state' resource.
Incoming gamepad events are broadcast in ASCII on the
'events' resource.


RESOURCES
device : The full path to the Linux joystick device to
use.  Changing this causes the old device to be closed
and the new one opened.  The default value of 'device'
is /dev/input/js1.

events : A broadcast resource that outputs gamepad events
as they arrive.  This can be useful for debugging or if
you want to watch for one specific event.  The output of
events is ASCII text terminated by a newline with one
line per event.  The timestamp is in milliseconds and
events are either an axis change (A) or a button change
(B).  The form of the output is:
    timestamp <A|B> ID value
For example, pressing the 'Start' button on the gamepad
would generate an event similar to this:
    80114284 B 7 1

filter : A hex value that specifies which values to
display as part of 'state'.  A set bit filters out
that gamepad control.  The bits in the hex value have
the following meaning:
   000001 : 'A' button
   000002 : 'B' button
   000004 : 'X' button
   000008 : 'Y' button
   000010 : Left top button
   000020 : Right top button
   000040 : Left center button
   000080 : Right center button
   000100 : Top center button
   000200 :    (unused)
   000400 :    (unused)
   000800 :    (unused)
   001000 :    (unused)
   002000 :    (unused)
   004000 :    (unused)
   008000 :    (unused)
   010000 : Left horizontal joystick
   020000 : Left vertical joystick
   040000 : Left trigger
   080000 : Right horizontal joystick
   100000 : Right vertical joystick
   200000 : Right trigger
   400000 : Horizontal hat switch
   800000 : Vertical hat switch

state : A broadcast resource that outputs the filtered
state of the gamepad every 'period' millisecond (or on
state change if period is zero).  The state has up to
eight axis values and sixteen button events.  The form
of the output with all controls unfiltered is:
    timestamp buttons ax1 ax2 ax3 ax4 ax5 ax6 ax7 ax8
where 'buttons' is the hex value of the state of all
buttons, and the axis value are signed decimal value
show the state of the axis.  The timestamp is for the
last event reported.  If the timestamp is not changing
it is because there are no new events.

period : A read-write resource that sets the period in
milliseconds between updates to 'state'.  Any integer
value greater than or equal to zero is accepted but is
rounded off to the next highest 10 milliseconds. If the
period is zero a new state is broadcast when a new event
arrives.  If non-zero the state is broadcast every 
'period' milliseconds whether or not new events have
arrived.

EXAMPLE
  Display the right vertical joystick value and the top
left button.  Display on value change only (no periodic
updates).
   hbaset gamepad filter efffef
   hbaset gamepad period 0 
   hbacat gamepad state
