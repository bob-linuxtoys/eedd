RTC: Real-Time Clock
The Real-Time Clock peripheral and battery-backed RTC card
let you keep time when your device is powered off. when
used with one of the DPI power distribution cards, the RTC
peripheral and card let you set a wake time at which power
is restored to your system.


HARDWARE:
The RTC peripheral must be paired with the RTC card. The
card contains a NXP PCF2123 RTC chip and a CR2032 battery.
You can set and get time from the card, and you can enable
and set a wakeup time. An open-drain output on the card
goes low when the alarm time is reached. This output is
meant to connect to the remote input on a power
distribution card. The combination of the RTC card and
PD25 or PD15 card lets you power down your robot and have
it wake up at a later time.


RESOURCES:
time : the current time in the RTC chip. Use dpset to set
the time and dpget to retrieve it. The time format is:
    year-month-day hour:minute:second
For example:
    2018-09-10 14:42:00

alarm : the day and time at which the alarm output on the
RTC card will go low.  The alarm time is always within one
month of the current date.  The alarm output goes low only
if the state is 'enabled'.  This resource works with both
dpset and dpcat.  The format for the alarm time is:
    day hour:minute
Where 'day' is in the range 1 to 31 and is the month day
when the alarm is triggered.

state : the state of the alarm output.  Using dpset you can
set the state to:
  off     : the alarm output is floating
  on      : the alarm output if forced low
  enabled : the system is armed and waiting for an alarm
Reading the state also allows a state of:
  alarm   : an alarm occurred and the alarm output is low


NOTES:
  One use of the RTC card is to put the system to sleep and
have it wake up at a preset time. You would normally turn 
the system on with the switch on the power distribution card
and then enable the alarm on the RTC card.  When you are
ready to put the system to sleep turn it off using the power
distribution card switch. 
  If you wish to have the system automatically wake, perform
some action, and go back to sleep, then you need to set the
state to 'enabled' when you're ready to have the system go
back to sleep.
 

EXAMPLES:

Set the date and time on the RTC card from system time
    dpset rtc time `date --rfc-3339='seconds' -u`

Set system date from RTC clock time 
    sudo date --rfc-3339='seconds' -u -s `dpget rtc time`

Set the alarm time and enable the alarm.
    dpset rtc alarm 11 02:00:00
    dpset rtc state enabled  # machine turns off 
