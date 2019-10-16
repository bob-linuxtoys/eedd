HARDWARE
    The QPOT card has a Microchip MCP4251 quad 10K pot.
It has a 257 steps from 0 to 10K Ohms and has a wiper
resistance of 75 Ohms.  The QPOT card uses the ESPI 7474
circuit for reliable operation.


RESOURCES

value :
The potentiometer resistances as a percentage of full scale.
The setting are mapped to the closest percentage possible with 257
steps.  The actual value might not match the exact value you specify.
You can read the value resource to get the actual values chosen.



EXAMPLE
Set Pots 0 to 3 to 25, 50, 75, and 100 percent of full scale
        dpset qpot value 25.0 50.0 75.0 100.0

