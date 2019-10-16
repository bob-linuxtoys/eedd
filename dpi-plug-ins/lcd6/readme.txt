HARDWARE
   The lcd6 peripheral provides an LCD display with six 7-segment digits.


RESOURCES
   You can write directly to the segments of the display or write a six
character text message to it.  Characters for the text message must be
taken from the following set:
	0 1 2 3 4 5 6 7 8 9
	A b C d E F  (may be given as upper or lower case)
	o L r h H - u
	(space) (underscore) (decimal point)

display : Six 7-segment digits
   Characters written to this node are displayed.  The characters 
must be taken from the above set and only the first six characters
of the input line are displayed.  The exception to this are decimal
points which are displayed between the characters and which do not
count toward the six character limit.  Messages with less than six
characters are left justified.  Some examples if the display is in
slot 2:
	# display 123456
        telnet localhost 8880
	dpset 6 display 123456
	# display 8.8.8.8.8
	dpset 6 display 8.8.8.8.8
	# This will display 3.14156 -- discarding the extra digits
	dpset 6 display 3.1415926
	# Display 12 left justified
	dpset 6 display 12

segments : Individual segment control
   You can directly control which segments are displayed by writing
six space-separated hexadecimal values to the segments resource.
The MSB of each value controls the 'a' segment and the next-MSB value
controls the 'b' segment.  The LSB controls the decimal point.  For
example:
	# display the middle bar (segment g)  on the first three
	# digits and the leftmost two vertical bars (segments 'e'
	# and 'f') on the last three digits
	dpset 6 segments 80  80  80  60  60  60

