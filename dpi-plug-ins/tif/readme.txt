============================================================
TEXT INTERFACE CARD

HARDWARE
   The Text Interface Card has a rotary encoder with center
button, a piezo speaker, two user controlled LEDs, a 4x5
keypad interface, and an interface to a text LCD display.


RESOURCES
keypad : The state of the keypad.  No key down is reported
    as 00.  A non-zero value reports the column and
    row of the closure.  The column is the high nibble and
    is in the range of 1 to 4.  The row is the low nibble
    and is in the range of 1 to 5.  Looking at the card with
    keypad connector at the top, the pins are arranged as
         _COLUMN_ || ___ROW___
         1 2 3 4  || 1 2 3 4 5

encoder : The rotary count and center button status of the
    rotary encoder.  The count is positive for clockwise
    rotation and negative for counter-clockwise rotation.
    The count is incremental and report just the change since
    the last report.  To get a position from the rotary
    encoder you will need to sum the counts as they come in.
    There are 96 counts per full rotation.  Note that the
    encoder is mechanical and turning the knob too quickly
    might not allow the contacts to close.  This will appear
    as missing counts.
        Pressing the center button on the encoder give a
    button value of 1.  A zero indicates that it is open.

tone : The tone to play as space separated triple of duration
    in milliseconds, the frequency as digit, and 1 bit volume
    setting.  Duration must be positive and is limited to a
    maximum of 310 milliseconds.  Frequency is a digit in the
    range of 1 to 4 with the following meaning:
        1: 1454 Hz
        2: 726 Hz
        3: 484 Hz
        4: 363 Hz
    The piezo responds best at the higher frequencies and the
    low frequencies will not sound as loud.
        A typical invocation might appear as:
            dpset tif tone 300 1 1

leds : The state of the LEDs as a hex value between 0 and 7.
    Bit 0 controls the LCD backlight.  Bit 1 controls User
    LED #1, and Bit 2 controls User LED #2.  A set bit turns
    the LED on.

text : Printable ASCII characters to send to the text display.
    Text LCD displays do not scroll automatically.  To work
    around this the driver moves the cursor to the other line
    every 16 characters.  (20 character display?  Change WIDTH
    in the tif driver.)  A typical invocation would write out
    all 16 character of the line at once.  For example:
        dpset tif text 'Hello, World!   '

commands : Commands to the display as a string of two digit hex
    characters.  Commands let you initialize the display, set
    the cursor position, and control whether the cursor is
    visible or not.  Commands are specific to the LCD controller
    in the display module but common commands include:
        38 : 8 bit interface, 2 line display, 5x7 font
        0f : Display on, cursor on, blink cursor
        01 : Clear display, move cursor to home position
        80 : Move cursor to start of first line
        8x : Move cursor to position x on first line
        c0 : Move cursor to start of second line
        cx : Move cursor to position x on second line
     Some displays are much slower than others and you may find
     that you need to repeat a command in order to give it a
     little extra time to execute.  This does not seem to be a
     problem for sending text to the display.  As an example,
     a reasonable initialization command might be:
         dpset tif commands 38 0c 04 01




