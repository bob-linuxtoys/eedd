============================================================

Aamp can control the volume of an audio amplifier and mute
or unmute it.  It also controls the on/off state of an LED.
The amplifier is initialized to be enabled and set to the 
lowest volume and the LED is initialized to be off.

NOTE: Volume changes are not instantaneous, that is, if a
volume change is invoked then quickly read back, it is the 
actual current volume which will be returned, which may not 
be the value requested.


HARDWARE
    Aamp is designed to work with the AAMP audio amplifier
card.  Pin 1 controls the shutdown control input of the 
amplifier, effectively muting or unmuting it, i.e. turning 
it off or on.  Pins 2 and 3 control the volume up and down, 
respectively.  Pin 4 controls the LED.  


RESOURCES
volume : a command to set the volume of the amplifier.
The volume is specified as a number between 1 (lowest)
and 32 (highest), as described in the following table:

          Volume Control (dB)
  value Gain    value Gain    value Gain         
   32    24      21    7.5     10   -10             
   31    22.5    20    6        9   -12             
   30    21      19    4.5      8   -14             
   29    19.5    18    3        7   -16             
   28    18      17    1.5      6   -18             
   27    16.5    16    0        5   -20             
   26    15      15   -1.5      4   -22               
   25    13.5    14   -3        3   -24              
   24    12*     13   -4.5      2   -26              
   23    10.36   12   -6        1   -80           
   22    9       11   -8        

enabled : a command to enable (1) or disable (0) the amplifier.
Note that a volume change can only occur when the amplifier is
enabled.

led : a command to either turn the LED on (1) or off (0).


