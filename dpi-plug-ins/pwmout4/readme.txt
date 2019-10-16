
HARDWARE
  The pwmout4 peripheral provides four channels of 12 bit
PWM output.  The range is zero percent to one-hundred
percent duty cycle.  PWM values are specified in hex and
range from 0000 to 1000.  The period is the same for all
PWM outputs and there is no guaranteed phase relation
between the outputs.

RESOURCES
You can specify the PWM clock frequency and the output
duty cycles using the config and pwms resources.

config:
   The config resource specifies the PWM clock frequency.
The period of the PWM pulses is the frequency divided by
4096.  The config frequency must be one of the following:
   2000000 -- 2 MHz
   1000000 -- 1 MHz
    500000 -- 500 KHz
    100000 -- 100 KHz
     50000 --  50 KHz
     10000 --  10 KHz
      5000 --   5 KHz
      1000 --   1 KHz
       500 -- 500 Hz
       100 -- 100 Hz
        50 --  50 Hz

pwms:
    Specify the PWM duty cycles as hex numbers in the range of
0 to 1000.  A value of 800 is fifty percent duty cycle.  The
maximum is 1000 and the minimum is 0.



Examples:
    # Set the PWM clock to 20 MHz
    dpset pwmout4 config 20000000
    # Set the PWM duty cycles to 25, 50, 75 and 100 percent
    dpset pwmout4 pwms 400 800 c00 1000



