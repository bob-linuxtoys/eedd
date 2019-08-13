
HARDWARE
  The GPS peripheral gives easy access to the location
data from a GPS receiver connected to a serial port.  

RESOURCES
You can specify the baud rate and which serial port
to use.  The satellite lock status and the number of
locked satellites is available using dpget.  A stream
of location data is available using dpcat.

config:
   The baudrate to use for the serial port and the
full path to the serial port's /dev entry.  The
config resource works with dpset and dpget.

status:
   The receiver status and the number of satellites
in use.  A -1 indicates that the serial port is not
open, a 0 indicated an open serial port but no GPS
lock, and a 1 indicates a valid GPS lock.  The status
resource works with dpget.

tll:
   Time, longitude and latitude in degrees.  Location
is updated once a second if the receiver is locked
to a sufficient number of satellites.  Time is the
number of seconds since midnight UTC.  


EXAMPLES
Configure the system for 4800 baud and ttyUSB1
    edset gps config 4800 /dev/ttyUSB1

Get the status of the receiver
    edget gps status

Start a stream of location data
    edcat gps tll



