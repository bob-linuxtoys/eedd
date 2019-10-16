============================================================

HARDWARE
   The DC2 peripheral controls the direction and percent
power of two brushed DC motors using the D7HB H-Bridge card.
The motor power is pulse width modulated (PWM) with a duty
cycle between 0 and 100 and programmable PWM frequency.


RESOURCES

mode0, mode1 : mode of operation for the motor.
The mode is encoded as a single character with the codes 
assigned as follows:
     b -- brake  (high-side dynamic brake) Power-on-Default
     f -- forward
     r -- reverse
     c -- coast  (non-synchronous regenerative brake)

power0, power1 : PWM 'ON' time as a percentage.
The power resolution is 10 bits for PWM frequencies below 20
KHz.  The power resolution is 8 bits for a PWM frequency of
78 KHz.  

pwm_frequency : PWM frequency in Hertz.
The driver tries to assign a frequency as close as possible to
the one specified.  For example, a requested frequency of 12510
Hertz might result in an actual PWM frequency of 12500 Hertz.
The PWM frequency is the same for both motors.  The default PWM
frequency is 20000 Hertz.

watchdog : Timeout in milliseconds.
As a safety feature, the FPGA hardware can turn both motors off
if there is no speed or mode update within the specified time.
The time is specified in milliseconds and has a maximum value
of 1500 milliseconds.  The resolution of the watchdog timer is
100 milliseconds, and values must be specified in multiples of
100.  A value of zero turns off the timer, and the default value
is zero.
