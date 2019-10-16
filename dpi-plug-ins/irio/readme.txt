============================================================

This peripheral can receive and send IR packets using the
'NEC Protocol' which is the most common one in use.  This
protocol typically has a 16 bit address followed by an 8
bit command followed by the command again but with the bits
inverted.  

You can find a list of devices, their addresses, and commands
here: http://lirc.sourceforge.net/remotes/.  The address is
given as the 'pre_data' value and the commands are listed by
function and value.  Note that the second byte of most commands
is the inverse of the first byte.


RESOURCES
recv : Received IR packets given as a newline terminated 32
bit hexadecimal string.  This resource works with dpcat but
not with dpget or dpset.

xmit : 32 bit hex value to be transmitted.  This resource
works with dpset but not dpget.

