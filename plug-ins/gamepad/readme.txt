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

period : A read-write resource that sets the period in
milliseconds between updates to 'state'.  Any integer
value greater than or equal to zero is accepted but is
rounded off to the next highest 10 milliseconds. If the
period is zero a new state is broadcast when a new event
arrives.  If non-zero the state is broadcast every 
'period' milliseconds whether or not new events have
arrived.

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

state : A broadcast resource that outputs the entire
state of the gamepad every 'period' millisecond (or on
state change if period is zero).  The state has up to
eight axis values and sixteen button events.  The form
of the output is:
    timestamp buttons ax1 ax2 ax3 ax4 ax5 ax6 ax7 ax8
where 'buttons' is the hex value of the state of all
buttons, and the axis value are signed decimal value
show the state of the axis.  The timestamp is for the
last event reported.  If the timestamp is not changing
it is because there are no new events.

