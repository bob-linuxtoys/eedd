HARDWARE
The quad Parallax Ping interface makes each pin on the connector
into a channels of bidirectional I/O that connects directly to a
Parallax Ping ultrasonic range sensor..  The first channel appears
on the BaseBoard connector pin 2 or on pin 10 depending on the slot
number for the peripheral.  The fourth channel is on pin 8 or 16.
The ultrasonic pings are sent out sequentially so that they do not
interfere with each other.


RESOURCES
distance : Sensor ID and Distance
   The distance reported by each sensor as the readings come in.
The distance resource works with dpcat and several programs can
monitor at the same time.
   The output is a digit that indicates the sensor number followed
by the distance given in units of one-tenth of an inch.  The sensor
ID and readings are separated by a space and terminated by a newline.
A distance of zero indicates that the sensor is not connected or is
not responding.
   Typical output from the distance resource might appear as:
     0 1055
     1 55
     2 0
     3 1848
     0 1094


enable : Enable or disable individual sensors
   A single hexadecimal character terminated by a newline that can
turn individual sensor on or off. The LSB of the enable register 
corresponds to the lowest numbered sensor, 0.  A set bit enables
the sensor and a cleared bit disables it.  You may wish to disable
unused sensors since their absence slows the sampling process.
At start-up all sensors are disabled.

NOTE: ONLY ENABLE ACTIVE SENSORS. THE OUTPUT CAN STOP IF A SENSOR
IS NOT PRESENT.


