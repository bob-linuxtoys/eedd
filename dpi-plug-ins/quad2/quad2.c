/*
 *  Name: quad2.c
 *
 *  Description: Dual quadrature decoder driver
 *
 *  Hardware Registers:
 *    0,1:     - quadrature count #0 as a signed 16 bit two's complement number
 *    2,3:     - usec timestamp of the most recent count
 *    4,5:     - quadrature count #0 as a signed 16 bit two's complement number
 *    6,7:     - usec timestamp of the most recent count
 *    8  :     - Poll interval in units of 10ms.  0-5, where 0=10ms and 5=60ms, 7=off
 * 
 *  Resources:
 *    counts      - quadrature counts as a signed decimal number terminated by a newline
 *    update_period - update period in ms. _0_ is off (translate to 7 for hardware).
 */

/*
 * Copyright:   Copyright (C) 2014-2019 Demand Peripherals, Inc.
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
 * 
 */

/*
 *    The dual quadrature decoder provides two independent channels of 
 *    quadrature decoding suitable for use with high speed wheel encoders
 *    or low speed user interface controls.  The quadrature counts are
 *    given as 16-bit two's complement signed numbers and each count is
 *    accompanied by a timestamp that is the number of microseconds into
 *    the sample period when the most recent countable edge occurred.
 *      
 *    The minimum update period is 10 milliseconds and the maximum is 60ms.
 *    The maximum input clock frequency is one megahertz.
 *
 *    The period computation deserves a little explanation.  The timestamp
 *    register is a timestamp in microseconds of the last edge counted in
 *    the sample interval.  
 *       For example, say the update rate is 10 ms and that the previous
 *    sample had a timestamp of 8000 microseconds.  If the current sample
 *    has a count 2 and a timestamp of 6000 microseconds, then we can compute
 *    the frequency as follows:
 *       interval = 6000 + (10000 - 8000) (previous timestamp) = 8000 usec
 *       frequency = 2 / 8000us = 250 Hz
 *    Note that the accuracy of the frequency is fairly high since the 
 *    interval measurement is so accurate.  For high count rates (>1000)
 *    you might want to use the sample time as the sample interval.
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
        // QUAD2 register definitions
#define QUAD2_REG_COUNT0    0x00
#define POLL_RATE_REG       0x08
        // resource names and numbers
#define FN_COUNTS           "counts"
#define FN_RATE             "update_period"
#define RSC_COUNTS          0
#define RSC_RATE            1


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an quad2
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    float    tstamp0;  // accumulated period from previous samples
    float    tstamp1;  // accumulated period from previous samples
    uint8_t  period;   // update rate for hardware sampling 0=10ms, 1=20ms, 7=off
    void    *ptimer;   // timer to watch for dropped ACK packets
} QUAD2DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userperiod(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, QUAD2DEV *);
static void sendconfigtofpga(QUAD2DEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    QUAD2DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (QUAD2DEV *) malloc(sizeof(QUAD2DEV));
    if (pctx == (QUAD2DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in quad2 initialization");
        return (-1);
    }

    // Init our QUAD2DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->period = 7;          // default value matches power up default==off
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->tstamp0 = 0;
    pctx->tstamp1 = 0;

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_COUNTS].name = FN_COUNTS;
    pslot->rsc[RSC_COUNTS].flags = CAN_BROADCAST;
    pslot->rsc[RSC_COUNTS].bkey = 0;
    pslot->rsc[RSC_COUNTS].pgscb = 0;
    pslot->rsc[RSC_COUNTS].uilock = -1;
    pslot->rsc[RSC_COUNTS].slot = pslot;
    pslot->rsc[RSC_RATE].name = FN_RATE;
    pslot->rsc[RSC_RATE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_RATE].bkey = 0;
    pslot->rsc[RSC_RATE].pgscb = userperiod;
    pslot->rsc[RSC_RATE].uilock = -1;
    pslot->rsc[RSC_RATE].slot = pslot;
    pslot->name = "quad2";
    pslot->desc = "Dual Quadrature Decoder";
    pslot->help = README;


    // Send the value, direction and interrupt setting to the card.
    // Ignore return value since there's no user connection and
    // system errors are sent to the logger.
    sendconfigtofpga(pctx, (int *) 0, (char *) 0);  // send pins, dir, intr

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,    // handle for our slot's internal info
    DP_PKT  *pkt,      // the received packet
    int      len)      // number of bytes in the received packet
{
    QUAD2DEV *pctx;    // our local info
    RSC      *prsc;    // pointer to this slot's counts resource
    int16_t   count0;  // quad count #0 for this sample interval
    int16_t   count1;  // quad count #1 for this sample interval
    uint16_t  ts0;     // timestamp for counter 0
    uint16_t  ts1;     // timestamp for counter 1
    float     period0; // interval of last set of counts
    float     period1; // interval of last set of counts
    char      qstr[150];  // two signed numbers and two floats
    int       qlen;    // length of quadrature count string
    uint16_t  sample_usec; // usec in the current sample period


    pctx = (QUAD2DEV *)(pslot->priv);  // Our "private" data is a GPIO4DEV
    prsc = &(pslot->rsc[RSC_COUNTS]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  // Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the counters should come in since we don't ever read the period
    // (four 16 bit numbers takes _8_ bytes.)
    if ((pkt->reg != QUAD2_REG_COUNT0) || (pkt->count != 8)) {
        edlog("invalid quad2 packet from board to host");
        return;
    }

    // 10000 usec in 10 ms.  
    sample_usec = (pctx->period + 1) * 10000;

    // Get counts and timestamps
    count0 = (pkt->data[0] << 8) + pkt->data[1];
    ts0    = (pkt->data[2] << 8) + pkt->data[3];
    count1 = (pkt->data[4] << 8) + pkt->data[5];
    ts1    = (pkt->data[6] << 8) + pkt->data[7];
    if (pkt->data[0] & 0x80) {
        count0 = -(1 << 16) + count0;
    }
    if (pkt->data[4] & 0x80) {
        count1 = -(1 << 16) + count1;
    }
    // ignore count timestamp if count is zero
    if (count0 == 0) {
        period0= 0.0;
        // accumulate time for <1 counts per sample
        pctx->tstamp0 += sample_usec/1000000.0;
    }
    else {
        period0 = pctx->tstamp0 + ts0/1000000.0;  // in sec
        pctx->tstamp0 = (sample_usec - ts0) / 1000000.0;
    }
    if (count1 == 0) {
        period1= 0.0;
        // accumulate time for <1 counts per sample
        pctx->tstamp1 += sample_usec/1000000.0;
    }
    else {
        period1 = pctx->tstamp1 + ts1/1000000.0;  // in sec
        pctx->tstamp1 = (sample_usec - ts1) / 1000000.0;
    }

    // Process of elimination makes this an autosend packet.
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey != 0) {
        qlen = sprintf(qstr, "%4d %3.6f %4d %3.6f\n", count0, period0, count1, period1);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(qstr, qlen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * userperiod():  - The user is reading or setting the sample period
 **************************************************************/
static void userperiod(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    QUAD2DEV *pctx;    // our local info
    int      ret;      // return count
    int      newperiod;  // new value to assign the direction

    pctx = (QUAD2DEV *) pslot->priv;

    if (cmd == EDGET) {
        // 0=10ms, 1=20ms, ...5=60ms, 7=off(shown as 0ms)
        ret = snprintf(buf, *plen, "%d\n", ((pctx->period + 1) % 8) * 10);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%d", &newperiod);
        if ((ret != 1) || (newperiod < 0) || (newperiod > 60)) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // 0ms is off but is sent to hardware as a 7
        pctx->period = (newperiod != 0) ? (newperiod / 10) - 1 : 7 ;

        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send sample period to the FPGA card. 
 * Put error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    QUAD2DEV *pctx,    // This peripheral's context
    int      *plen,    // size of buf on input, #char in buf on output
    char     *buf)     // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the quad2
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Write the values for the pins, direction, and interrupt mask
    // down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = POLL_RATE_REG;   // the first reg of the three
    pkt.count = 1;
    pkt.data[0] = pctx->period;
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    if (txret != 0) {
        // the send of the new pin values did not succeed.  This
        // probably means the input buffer to the USB port is full.
        // Tell the user of the problem.
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    QUAD2DEV *pctx)    // Send pin values of this quad2 to the FPGA
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of quad2.c
