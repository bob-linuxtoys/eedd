/*
 * Name: ei2c.c
 * 
 * Description: General purpose I2C interface
 *
 * Copyright:   Copyright (C) 2017-2019 by Demand Peripherals, Inc.
 *              All rights reserved.
 *
 * License:     This program is free software; you can redistribute it and/or
 *              modify it under the terms of the Version 2 of the GNU General
 *              Public License as published by the Free Software Foundation.
 *              GPL2.txt in the top level directory is a copy of this license.
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *              GNU General Public License for more details. 
 *
 *              Please contact Demand Peripherals if you wish to use this code
 *              in a non-GPLv2 compliant manner. 
 */

/*
 *    The ei2c peripheral card provides an I2C interface at either 400 or
 *  100 KHz clock and with up to 128 bits of packet data.  Start, stop,
 *  ACK bits are included in the 128 bits limiting the total number of bits
 *  available for user data.
 *
 *  Registers:
 *    The ei2c peripheral uses 128 8-bit registers to send and receive I2C
 *  bits.  The MSB is the speed (set=400), the LSB is the data bit, and 
 *  bit 1 specifies the type of the bit.  The data bit is set high for
 *  reads.  The definition are:
 *     Bit 1 / 0  Meaning
 *         0 / 0  Write a zero bit to I2C device
 *         0 / 1  Write a one bit or read the device's data
 *         1 / 0  Send a START bit
 *         1 / 1  Send a STOP bit.  End of packet.
 * 
 */


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "eedd.h"
#include "readme.h"




/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // misc constants
#define MXLINE            100
#define NI2CBITS          128
#define NI2CBYTES          13
#define DEFI2CSPEED       100
// Types of bits
#define  I2START            2
#define  I2STOP             3
#define  I2READ             1
#define  I2WRITE0           0
#define  I2WRITE1           1
#define  ACK                1
        // Resource index numbers
#define RSC_DATA            0
#define RSC_CFG             1
#define FN_CFG              "config"
#define FN_DATA             "data"


// user node data direction
#define READ 0                  // read from user
#define WRITE 1                 // write to user


// local context
typedef struct
{
    SLOT    *pslot;        // handle to peripheral's slot info
    int      flowCtrl;     // == 1 when backpressure from USB port
    int      xferpending;  // ==1 if we are waiting for a reply
    void    *ptimer;       // Watchdog timer to abort a failed transfer
    int      speed;        // bus speed in KHz.  Must be 400 or 100
    int      hex[NI2CBYTES];        // command bytes as integers (-1 == Read)
    int      nbytes;       // number of bytes in command
    int      nbits;        // number of bits in I2C packet
} EI2CDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void  packet_hdlr(SLOT *, DP_PKT *, int);
static void  cb_data(int, int, char*, SLOT*, int, int*, char*);
static void  cb_config(int, int, char*, SLOT*, int, int*, char*);
static int   send_i2c(EI2CDEV *);
static void  no_ack(void *, EI2CDEV *);
static void  seti2cbyte(unsigned char *, int);
static int   geti2cbyte(unsigned char *);
extern int   dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    EI2CDEV  *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (EI2CDEV *) malloc(sizeof(EI2CDEV));
    if (pctx == (EI2CDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in ei2c initialization");
        return (-1);
    }

    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->speed = DEFI2CSPEED; // set a default I2C bus speed


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_DATA].name = FN_DATA;
    pslot->rsc[RSC_DATA].flags = IS_READABLE;
    pslot->rsc[RSC_DATA].bkey = 0;
    pslot->rsc[RSC_DATA].pgscb = cb_data;
    pslot->rsc[RSC_DATA].uilock = -1;
    pslot->rsc[RSC_DATA].slot = pslot;
    pslot->rsc[RSC_CFG].name = FN_CFG;
    pslot->rsc[RSC_CFG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CFG].bkey = 0;
    pslot->rsc[RSC_CFG].pgscb = cb_config;
    pslot->rsc[RSC_CFG].uilock = -1;
    pslot->rsc[RSC_CFG].slot = pslot;
    pslot->name = "ei2c";
    pslot->desc = "a generic I2C interface";
    pslot->help = README;

    return (0);
}

/**************************************************************
 * Handle incoming packets from the peripheral.
 * Check for unexpected packets, discard write response packet,
 * send read response packet data to UI.
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    RSC     *prsc;
    EI2CDEV *pctx;
    char     buf[MXLINE];
    int      lix = 0;            // length of string in buf
    int      pix = 0;            // packet index
    int      bytval;

    prsc = &(pslot->rsc[RSC_DATA]);
    pctx = (EI2CDEV *)(pslot->priv);

    // verify write response packets
    if (((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) &&
        (pkt->count == pctx->nbits)) {
        return;
    }

    // handle asynchronous status from the peripheral
    if (((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) ||
        (pkt->count > NI2CBITS) ||
        (pkt->count < 10))           // minimum of start,stop, and 1 byte
    {
        // unknown packet
        edlog("invalid ei2c packet from board to host");
        return;
    }

    // we have a valid ei2c response.  give it to the user
    // The first character in the reply is for the Ack or Nak (A or N)
    // We start with the assumption it is an A and overwrite if any NAKs
    lix = 0;
    buf[lix++] = 'N';
    buf[lix++] = ' ';

    // The first 8 bits have the Start bit and the address.  Get this
    // as a byte and lop off the Start bit
    pix = 0;
    bytval = geti2cbyte(&(pkt->data[pix]));
    lix += snprintf(&(buf[lix]), MXLINE-lix, "%02x ", (bytval & 0x7F));
    pix += 8;

    // The next bit in the response indicate whether this was a read or write
    buf[lix++] = (pkt->data[pix++] & 1) ? 'r' : 'w';
    buf[lix++] = ' ';

    // The next bit is the inverted NACK for the address.  Remember 0 is an ACK
    buf[0] = (pkt->data[pix++] & 1) ? buf[0] : 'A';

    // loop through the remaining bytes watching for a write-to-read transition
    while (pix < pkt->count) {
        if (pix > pkt->count - 10) {  // sanity check
            edlog("invalid ei2c packet from board to host");
            return;
        }

        // Test for a START bit
        if (pkt->data[pix] == I2START) {
            pix += 8;    // skip from START to read/write bit
            buf[lix++] = (pkt->data[pix++] & 1) ? 'r' : 'w';
            buf[lix++] = ' ';
        } else {
            // Get and print the byte
            bytval = geti2cbyte(&(pkt->data[pix]));
            lix += snprintf(&(buf[lix]), MXLINE-lix, "%02x ", bytval);
            pix += 8;
        }

        // The next bit is the (inverted NACK for the byte
        buf[0] = (pkt->data[pix++] & 1) ? buf[0] : 'A';

        // Check for the STOP bit
        if (pkt->data[pix] == I2STOP) // we're done
            break;
    }

    lix += snprintf(&(buf[lix]), MXLINE-lix, "\n");

    // Send data to UI
    send_ui(buf, lix, prsc->uilock);
    prompt(prsc->uilock);

    // Response sent so clear the lock
    prsc->uilock = -1;
    del_timer(pctx->ptimer);  //Got the response
    pctx->ptimer = 0;
    return;

}


/*
 * geti2cbyte:-- convert series of LSBs to a byte.  
 */
static int geti2cbyte(unsigned char *pd)
{
    int value;

    value = *pd++ & 1;
    value = (value << 1) + (*pd++ & 1);
    value = (value << 1) + (*pd++ & 1);
    value = (value << 1) + (*pd++ & 1);
    value = (value << 1) + (*pd++ & 1);
    value = (value << 1) + (*pd++ & 1);
    value = (value << 1) + (*pd++ & 1);
    value = (value << 1) + (*pd++ & 1);
    return(value);
}

/**************************************************************
 * Callback used to handle config resource from UI.
 * Read dpset parameters and send them to the peripheral.
 * On dpget, return current configuration to UI.
 **************************************************************/
static void cb_config(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    int      newspeed = -1;
    int      outlen;
    char     obuf[MXLINE];

    RSC *prsc = &(pslot->rsc[RSC_DATA]);
    EI2CDEV *pctx = pslot->priv;

    if (cmd == EDSET) {
        if (sscanf(val, "%d\n", &newspeed) != 1) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // Sanity check on the input
        if ((newspeed != 400) && (newspeed != 100)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        pctx->speed = newspeed;
    }
    else {
        // write out the current configuration
        outlen = snprintf(obuf, MXLINE, "%d\n", pctx->speed);
        prsc->uilock = (char) cn;
        send_ui(obuf, outlen, prsc->uilock);
        prompt(prsc->uilock);
        prsc->uilock = -1;
    }

    return;
}


/**************************************************************
 * Callback used to handle data resource from UI.
 * Read dpget parameters and send them to the peripheral.
 * Response packets will be handled in packet_hdlr().
 **************************************************************/
static void cb_data(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    char      *pbyte;
    int        tmp;
    int        txret;
    EI2CDEV   *pctx;


    pctx = pslot->priv;

    if(cmd == EDGET) {
        // Get the bytes to send
        pctx->nbytes = 0;
        pbyte = strtok(val, ", ");
        while (pbyte) {
            if ( 1 == sscanf(pbyte, "%x", &tmp))
                pctx->hex[pctx->nbytes] = (unsigned char) (tmp & 0x00ff);
            else
                pctx->hex[pctx->nbytes] = -1;  //assume failed hex is an 'R'
            pbyte = strtok((char *) 0, ", ");   // commas or spaces accepted
            pctx->nbytes++;
            if (pctx->nbytes == NI2CBYTES)
                break;
        }
        if (pctx->nbytes != 0) {
            txret = send_i2c(pctx);
            if (txret != 0) {
                *plen = snprintf(buf, *plen, E_WRFPGA);
                // (errors are handled in calling routine)
                return;
            }

            // Start timer to look for a read response.
            if (pctx->ptimer == 0)
                pctx->ptimer = add_timer(ED_ONESHOT, 100, no_ack, (void *) pctx);

            // lock this resource to the UI session cn
            pslot->rsc[RSC_DATA].uilock = (char) cn;

            // Nothing to send back to the user
            *plen = 0;
        }
        else {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
    }

    return;
}


/*  
 * Send a packet to write the I2C command
 * Return the number of bytes left to send (i.e. zero normally)
 */         
static int send_i2c(EI2CDEV *pctx)
{       
    DP_PKT   pkt;
    SLOT    *pslot;
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;
    int      i;
    int      inread;    // set=1 if we are reading bytes from the slave dev

    pslot = pctx->pslot;       // our instance of a peripheral
    pmycore = pslot->pcore;

    // Sanity check:  we need at least two bytes and the first
    // byte has to be a hex value.
    if ((pctx->nbytes < 2) || (pctx->hex[0] == -1)) {
        edlog("invalid I2C packet");
        return(pctx->nbytes);
    }

    // See the protocol manual for a description of the registers.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = 0;
    pkt.count = 0;

    // We need a start bit to begin
    pkt.data[pkt.count++] = I2START;

    // if next byte is -1 we are doing a read, else write
    inread = (pctx->hex[1] == -1) ? 1 : 0 ;

    // Fill in the address and read/write bit
    seti2cbyte(&(pkt.data[pkt.count]), ((pctx->hex[0] << 1) | inread));
    pkt.count += 9;

    // loop through the rest of the bytes setting the bits to read or write
    for (i = 1; i < pctx->nbytes; i++) {
        if (pkt.count > (NI2CBITS - 10)) {  // check size before adding bytes
            edlog("invalid I2C packet");
            return(pctx->nbytes);
        }
        seti2cbyte(&(pkt.data[pkt.count]), pctx->hex[i]);
        pkt.count += 9;

        // Is there a next byte?  If so, the next byte may switch
        // us from a write to a read, and we will need to send
        // another start bit and address byte
        if (i == (pctx->nbytes -1))  // more bytes?
            continue;
        if ((inread && (pctx->hex[i+1] != -1)) ||     // read to write
            ((inread == 0) && (pctx->hex[i+1] == -1))) {    // write to read
            inread = (pctx->hex[i+1] == -1) ? 1 : 0 ;  // compute new inread
            if (pkt.count > (NI2CBITS - 11)) {  // check size before adding bytes
                edlog("invalid I2C packet");
                return(pctx->nbytes);
            }
            pkt.data[pkt.count++] = I2START;  // restart for read/write switch
            seti2cbyte(&(pkt.data[pkt.count]), ((pctx->hex[0] << 1) | inread));
            pkt.count += 9;
        }
    }
    pkt.data[pkt.count++] = I2STOP;
    pctx->nbits = pkt.count;

    // bit 7 of the first byte is the clock rate
    pkt.data[0] |= (pctx->speed == 400) ? 0x80 : 0x00;

    // try to send the packet.  Apply or release flow control.
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    return(txret);
}

static void seti2cbyte(unsigned char *pktdata, int newbyte)
{
    // Add 9 bytes to the packet to send.  Each bit in a I2C
    // byte uses one byte in the over-the-wire packet.  The
    // ninth byte for the and ACK bit.

    if (newbyte == -1) {   // read ?
        *pktdata++ = I2READ;  // 7
        *pktdata++ = I2READ;  // 6
        *pktdata++ = I2READ;  // 5
        *pktdata++ = I2READ;  // 4
        *pktdata++ = I2READ;  // 3
        *pktdata++ = I2READ;  // 2
        *pktdata++ = I2READ;  // 1
        *pktdata++ = I2READ;  // 0
        *pktdata++ = ACK;  // We always give an ACK to reads
    } else {
        *pktdata++ = I2WRITE0 | ((newbyte >> 7) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 6) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 5) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 4) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 3) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 2) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 1) & 1);
        *pktdata++ = I2WRITE0 | ((newbyte >> 0) & 1);
        *pktdata++ = ACK;  // read the ACK from the slave device
    }
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void no_ack(
    void     *timer,   // handle of the timer that expired
    EI2CDEV  *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}


// end of ei2c.c
