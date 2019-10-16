/******************************************************************************
*
*  Name: fifo.c
*
*  Description:
*    This is an example usage of the DP AVR peripheral's fifo resource.
*
*  Test case:  
*    Setup: screen /dev/ttyUSBn 38400
*    Command line: dpset avr fifo 0 5 6 7 8 9a bc de
*    TTY Output: 05 06 07 08 9A BC DE
*
******************************************************************************/

#define BAUD 38400
#include "../../include/dpavr.h"
#include "../../include/dpavrusart.h"

//#define DEBUG

// host-to-fifo buffer
#define TX_BUFFER hostRegs[0]   

// fifo definition
#define FIFO_DATA_MAX 20
typedef struct
{
    char head;
    char tail;
    unsigned char data[FIFO_DATA_MAX];
} FIFO;
int fifo_full(volatile FIFO* pFifo)
{
    return (((pFifo->head + 1) % FIFO_DATA_MAX) == pFifo->head);
}
int fifo_empty(volatile FIFO* pFifo)
{
    return (pFifo->head == pFifo->tail);
}
void fifo_push(volatile FIFO* pFifo, unsigned char value)
{
    pFifo->data[(int)pFifo->head] = value;
    pFifo->head = (pFifo->head + 1) % FIFO_DATA_MAX;
}
unsigned char fifo_pop(volatile FIFO* pFifo)
{
    unsigned char value = pFifo->data[(int)pFifo->tail];
    pFifo->tail = (pFifo->tail + 1) % FIFO_DATA_MAX;
    return value;
}

#ifndef DEBUG
volatile FIFO fifoTx = {0};
#else
// for debugging this will map the fifo onto the host registers
volatile FIFO* fifoTx = (FIFO*)&hostRegs[1];
#define fifoTx (*fifoTx)
#endif

// fifo set hook is called when the host writes a byte to a fifo
void FifoSetHook()
{
    // push the byte from xmit buffer into the fifo
    if (!fifo_full(&fifoTx))
    {
        fifo_push(&fifoTx, TX_BUFFER);
    }
}

int main()
{
    // init the USART and communications between the host and the AVR
    dpavr_init();
    dpavr_usart_init();
    
    // register the fifo set hook
    dpavr_register_fifo_set_hook(FifoSetHook);
     
    dpavr_println("fifo example");    
    while(1)
    {
        // pop and xmit a byte from the fifo if it's not empty
        if (!fifo_empty(&fifoTx))
        {
            dpavr_print_byte(fifo_pop(&fifoTx), 16);
            dpavr_print_string(" ");
        }
    }
    
    return 0;
}

