/******************************************************************************
*
*  File: dpavrusart.h
*
*  Description:
*
*
******************************************************************************/

#include <util/setbaud.h>

#ifndef BAUD
#define BAUD 9600
#endif

typedef void (*USER_RX_HOOK)(unsigned char);
USER_RX_HOOK UserRxHook = 0;

void dpavr_usart_init()
{
    // set baud
    UBRR0H = UBRRH_VALUE;
    UBRR0L = UBRRL_VALUE;
#if USE_2X
    UCSR0A |= (1 << U2X0);
#else
    UCSR0A &= ~(1 << U2X0);
#endif
    
    // enable receiver and transmitter
    UCSR0B = (1<<RXEN0) | (1<<TXEN0); 
       
    // set frame format: 8 data, 2 stop bits
    UCSR0C = (1<<USBS0) | (3<<UCSZ00);
}

ISR(USART_RX_vect)
{
    if (UserRxHook)
    {
        UserRxHook(UDR0);
    }
}

// received a byte from USART0
char dpavr_rx_byte()
{
    // wait for data to be received then return it
    while ( !(UCSR0A & (1<<RXC0)) )
    ;
    return UDR0;
}

// transmit a byte to USART0
void dpavr_tx_byte(char c)
{
    // wait for data buffer to empty then transmit it
    while (!( UCSR0A & (1<<UDRE0)))
    ;
    UDR0 = c;
}

// print the ASCII decimal form of an integer
void dpavr_print_byte(unsigned char byte, int radix)
{
    if (radix == 10)
    {
        dpavr_tx_byte('0' + ((byte / 10) % 10));
        dpavr_tx_byte('0' + (byte % 10));
    }
    else if (radix == 16)
    {
        unsigned char hextet;
        hextet = byte / 16;
        hextet = (hextet <= 9) ? '0' + hextet : 'a' + (hextet - 10);
        dpavr_tx_byte(hextet);
        hextet = byte % 16;
        hextet = (hextet <= 9) ? '0' + hextet : 'a' + (hextet - 10);
        dpavr_tx_byte(hextet);
    }
}

// transmit a string
void dpavr_print_string(char* s)
{
    int i = 0;
    while (s[i])
    {
        dpavr_tx_byte(s[i]);
        i++;
    }
}

// transmit a newline terminated string
void dpavr_println(char* s)
{
    char* crlf = "\r\n";
    dpavr_print_string(s);
    dpavr_print_string(crlf);
}

// transmit a string, value, and newline
void dpavr_debug_print(char* s, unsigned char value, int radix)
{
    char* crlf = "\r\n";
    dpavr_print_string(s);
    dpavr_print_byte(value, radix);
    dpavr_print_string(crlf);
}


// end of dpavrusart.h

