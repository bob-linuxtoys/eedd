INTRODUCTION
    The four pins on the WS28 card each control a string
of up to 64 RGBW or RGB WS2812 or SK6812 addressable LEDs.
Adafruit Industries makes the WS2812 popular under the
name 'neopixel'.


HARDWARE
    A signal to drive the WS2812 is available on the
lowest numbered pin.  However this pin is at 3.3 Volts
and can not drive a WS2812 directly.
    The WS28 card can drive up to four strings of 64 RGB
or RGBW LEDs.


RESOURCES
led : Which string and the hex value to write to that
LED string.  Use three bytes per LED for RGB and four
bytes per LED for RGBW LEDs.  The first parameter is
which of the four LED strings to address and the second
parameter is a sequence of hex characters to write to
the string.  The number of hex characters must be even
since LEDs have 3 or 4 bytes of LED data.


EXAMPLES
dpset ws28 led 1 ffffffffffffffffffffffffffffffffffffffffffffffff
dpset ws28 led 4 111111111111111111111111111111111111111111111111
dpset ws28 led 4 00ff00ff00000000ff00ff00ff00000000ff00ff00ff0000


