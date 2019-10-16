/******************************************************************************
*
*  File: dpavr.h
*
*  Description:
*
*
******************************************************************************/

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>

// NOTE: change this value to the number of host register reqired
#define HOST_REG_QTY 64

// PD7 is the LED port on the AVR daughter card
#define DPLED PD7

// user FIFO hooks
void (*UserFifoGetHook)() = 0;
void (*UserFifoSetHook)() = 0;

// AVR data memory operations
#define OP_RD       0x00
#define OP_WR       0x01
#define OP_MEM      0x00
#define OP_REG      0x02
#define OP_MEM_RD   OP_MEM | OP_RD
#define OP_MEM_WR   OP_MEM | OP_WR
#define OP_REG_RD   OP_REG | OP_RD
#define OP_REG_WR   OP_REG | OP_WR
#define OP_AUTOINC  0x04

// global SPI state used by the SPI ISR and reset by the PCI ISR
volatile unsigned spiState = 0;

// host register file used to allow host/AVR communications
volatile unsigned char hostRegs[HOST_REG_QTY];

// initialize pin change interrupts for SPI SS pin
void dpavr_pci_init()
{
    // enable pin change bank 0 interrupts
    PCICR |= (1<< PCIE0);
    
    // set the bank mask to only consider PB2/PCINT2, i.e. SPI SS
    PCMSK0 |= (1<< PB2);
}

// initialize SPI and its interrupts
void dpavr_spi_init()
{
    // make the AVR an SPI slave by setting MISO as output
    DDRB = (1<< PINB4);

    // enable SPI and its interrupt
    SPCR = (1<< SPE | 1<< SPIE);   
     
    // init data reg
    SPDR = 0;
}

void dpavr_init()
{
    dpavr_pci_init();
    dpavr_spi_init();
    sei();
}

// This ISR is called any time there is a change on the SPI SS line.
// It is used to determine when the SS line makes a low-to-high transition,
// which indicates that an SPI transaction is complete and the SPI state
// machine should be reset to its initial state, ready to accept new transactions.
ISR(PCINT0_vect)
{
    // reset the SPI state machine if the SS line transitions from low to hi
    if (bit_is_set(PINB, PB2))
    {
        spiState = 0;
    }
}

ISR(SPI_STC_vect)
{
    static unsigned char op;
    static int autoinc;
    static unsigned char hostRegIdx;
    static unsigned char *regAddr;
        
    switch (spiState)
    {
        // get the operation  
        case 0:
            op = SPDR & 0x03;
            autoinc = SPDR & OP_AUTOINC;
            switch (op)
            {
                case OP_MEM_WR:
                    spiState = 1;
                    break;
                case OP_MEM_RD:
                    spiState = 3;
                    break;
                case OP_REG_WR:
                    spiState = 5;
                    break;
                case OP_REG_RD:
                    spiState = 7;
                    break;
                default:
                    spiState = 0;
                    break;
            }
            break;

        // write a series of bytes to host register(s)
        case 1:
            // get the starting host register index
            hostRegIdx = (unsigned char)SPDR;
            spiState = 2;   
            break;                 
        case 2:
            // write a byte to a valid host register
            if (hostRegIdx < sizeof(hostRegs))
            {
                hostRegs[hostRegIdx] = (unsigned char)SPDR;
                hostRegIdx += (autoinc) ? 1 : 0;
                if (!autoinc && UserFifoSetHook)
                {
                    UserFifoSetHook();
                }
            }
            break;
            
        // read and return values from host register(s)
        case 3:
            // get the starting host register index
            hostRegIdx = (unsigned char)SPDR;
            spiState = 4;
            break;  
        case 4:
            // return a byte from a valid host register
            if (hostRegIdx < sizeof(hostRegs))
            {
                // load SPI data register first
                SPDR = hostRegs[hostRegIdx];
                
                // call get-hook next for fifos
                if (!autoinc && UserFifoGetHook)
                {
                    UserFifoGetHook();
                }
                
                // increment the reg index for autoinc
                hostRegIdx += (autoinc) ? 1 : 0;
            }
            break;

        // write a series of bytes to consecutive device registers            
        case 5:
            // get the starting register address
            regAddr = (unsigned char *)(0x0000 + SPDR);
            spiState = 6;
            break;            
        case 6:
            // write a byte to a device register
            *regAddr = (unsigned char)SPDR;
            regAddr += (autoinc) ? 1 : 0;
            break;
            
        // read and return values from consecutive device registers            
        case 7:
            // get the starting register address
            regAddr = (unsigned char *)(0x0000 + SPDR);
            spiState = 8;
            break;
        case 8:
            // return a byte from a valid device register
            SPDR = *regAddr;
            regAddr += (autoinc) ? 1 : 0;
            break;
            
        default:
            spiState = 0;
            break;
    }
}

void dpavr_register_fifo_get_hook(void(*fct)())
{
    UserFifoGetHook = fct;
}

void dpavr_register_fifo_set_hook(void(*fct)())
{
    UserFifoSetHook = fct;
}


// end of dpavr.h

