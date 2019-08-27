============================================================

VL53L0X Plug-in
The VL53L0X plug-in provides a simple ASCII interface
to the ST VL53L0X time of flight range sensor.
The plug-in has user configurable resources for the Linux 
device name, long range measurements, and period at which 
measurements are made.  Incoming range measurements are 
broadcast in ASCII on the 'range' resource.

NOTE: Only one VL53L0X plug-in can be loaded.  Loading
more than one instance of this peripheral will result 
in unexpected results.


RESOURCES

device : The full path to the range sensor device to
use.  The default value of 'device' is /dev/i2c-1.

hw_rev : The model and revision of the range sensor.
The form of the output is:
  <model ID> <revision ID>

longrange : Enable longer range measurements.  By default
long range is enabled, 30-2000mm.  Set to 0 to specify a shorter 
range of measurements, 30-800mm.  Note results beyond the range 
40-600mm are less accurate and more prone to being interfered 
with by ambient light.

period : The period in mSec in steps of 10 mSec at which 
measurements are made.  The default is 100 mSec which is 
the minimum.  The maximum is 5000, i.e. 5 seconds.

range : A broadcast resource that outputs range 
measurements at the specified period.  Each distances are 
measurement is returned as an ASCII integers terminated by a 
newline with one line per event.  The range measurements 
are in millimeters.

EXAMPLE
  Set the device to I2C channel 0:
   edset vl53 device /dev/i2c-1

  Get the HW revision of the range sensor:
   edget vl53 hw_rev

  Disable long range measurements:
   edset vl53 longrange 0

  Set the range measurement period:
   edset vl53 period 200

  Get a series of range measurements:
   edcat vl53 range

