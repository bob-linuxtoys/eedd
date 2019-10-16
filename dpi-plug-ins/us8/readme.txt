HARDWARE
    The us8 peripheral uses a us8 card to connect to up
to eight SRF-04 ultrasonic sensors.  The sensors have one
input pin and one output pin.  A 10 us pulse on the input
starts a ping.  The echo time of the ping is given as a
pulse width on the output pin.  The echo response starts
about 100 us after the end of the start pulse.  The FPGA
measures the pulse width of the echo reply and does an
auto-send of that time up to the host.  To avoid multiple
echoes the peripheral pings only one sensor at a time with
pings sent every 60 millisecond.
    The user can select which sensors are enabled.  The scan
rate per sensor is faster if you disable unused sensors.



RESOURCES
enable : Enable or disable individual sensors with a two
    digit hexadecimal number terminated by a newline that
    turns individual sensors on or off. The LSB of the enable
    register corresponds to the lowest numbered sensor, 0.
    A set bit enables the sensor and a cleared bit disables
    it.  You may wish to disable unused sensors since their
    presence slows the sampling process.  At start-up all
    sensors are disabled.


distance : Sensor ID and distance reported by each sensor
    as the readings come in.  The distance resource works
    with dpcat and several programs can monitor at the same
    time.  The output is a digit that indicates the sensor
    number followed by the distance in units of one-tenth
    of an inch.  The sensor ID and readings are separated
    by a space and terminated by a newline.  A distance of
    zero indicates that the sensor is not connected or is
    not responding.
   
    Typical output from the distance resource might appear as:
        0 1055
        1 55
        2 0
        3 1848
        0 1094

