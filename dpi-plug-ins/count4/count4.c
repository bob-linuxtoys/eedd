/*
 *  Name: count4.c
 *
 *  Description: Quad counter with count period
 *
 *  Hardware Registers:
 *    00,01: count0    - count #0 as a 16 bit unsigned integer
 *    02,03: period0   - timestamp of last count as number of usec into sample period
 *    04,05: count1    - count #1 as a 16 bit unsigned integer
 *    06,07: period1   - timestamp of last count as number of usec into sample period
 *    08,09: count2    - count #2 as a 16 bit unsigned integer
 *    10,11: period2   - timestamp of last count as number of usec into sample period
 *    12,13: count3    - count #3 as a 16 bit unsigned integer
 *    14,15: period3   - timestamp of last count as number of usec into sample period
 *    16               - update rate in range 10ms to 60ms
 *    17               - Edge selection as four 2-bit fields
 *         7,6 == counter3 edge selection, 0=off, 1=rising, 2=falling, 3=both
 *         5,4 == counter2 edge selection 
 *         3,2 == counter1 edge selection 
 *         1,0 == counter0 edge selection 
 * 
 *  Resources:
 *    update_rate - update rate in milliseconds. 
 *    counts      - counts and count intervals
 *    edges       - four ints in range of 0-3 to specify which edges to count
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
 *    The period computation deserves a little explanation.  The 'period'
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
#include <stdint.h>
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
        // COUNT4 register definitions
#define COUNT4_REG_COUNT0   0x00
#define COUNT4_REG_RATE     0x10
#define COUNT4_REG_EDGE     0x11
#define NCOUNT              4      /* four counters */

        // resource names and numbers
#define FN_COUNTS          "counts"
#define FN_RATE            "update_rate"
#define FN_EDGES           "edges"
#define RSC_COUNTS         0
#define RSC_RATE           1
#define RSC_EDGES          2


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an count4
typedef struct
{
    void    *pslot;       // handle to peripheral's slot info
    void    *ptimer;      // timer to watch for dropped ACK packets
    float    tstamp[NCOUNT];  // accumulated period from previous samples
    uint8_t  rate;        // update rate for hardware sampling
    uint8_t  edges;       // which edges to sample
} COUNT4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userparm(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, COUNT4DEV *);
static void sendconfigtofpga(COUNT4DEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)          // points to the SLOT for this peripheral
{
    COUNT4DEV *pctx;      // our local device context

    // Allocate memory for this peripheral
    pctx = (COUNT4DEV *) malloc(sizeof(COUNT4DEV));
    if (pctx == (COUNT4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in count4 initialization");
        return (-1);
    }

    // Init our COUNT4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->rate = 0;            // default value matches power up default
    pctx->edges = 0;           // default value matches power up default
    pctx->ptimer = 0;          // set while waiting for a response


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
    pslot->rsc[RSC_RATE].pgscb = userparm;
    pslot->rsc[RSC_RATE].uilock = -1;
    pslot->rsc[RSC_RATE].slot = pslot;
    pslot->rsc[RSC_EDGES].name = FN_EDGES;
    pslot->rsc[RSC_EDGES].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_EDGES].bkey = 0;
    pslot->rsc[RSC_EDGES].pgscb = userparm;
    pslot->rsc[RSC_EDGES].uilock = -1;
    pslot->rsc[RSC_EDGES].slot = pslot;
    pslot->name = "count4";
    pslot->desc = "Quad Event counter";
    pslot->help = README;

    return (0);
}


/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,     // handle for our slot's internal info
    DP_PKT  *pkt,       // the received packet
    int      len)       // number of bytes in the received packet
{
    COUNT4DEV *pctx;    // our local info
    RSC       *prsc;    // pointer to this peripheral's resources
    char       cstr[200];  // space for four sets of int and float
    int        ret;     // return value from sprintf to cstr
    int        i;       // counter under consideration
    int        clen;    // length of count output string
    uint16_t   count;   // counter counts
    uint16_t   sample_usec; // usec in the current sample period
    uint16_t   timestmp; // timestamp for this sample
    float      period;  // reported period for counts


    pctx = (COUNT4DEV *)(pslot->priv);  // Our "private" data is a GPIO4DEV
    prsc = &(pslot->rsc[RSC_COUNTS]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  // Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the counters should come in since we don't ever read the rate
    // (eight 16 bit numbers takes _32_ bytes.)
    if ((pkt->reg != COUNT4_REG_COUNT0) || (pkt->count != 16)) {
        edlog("invalid count4 packet from board to host");
        return;
    }

    // The sample period is a function of the sample rate
    sample_usec = (pctx->rate + 1) * 10000;

    // Process of elimination makes this an autosend packet.
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey != 0) {
        clen = 0;
        for (i = 0; i < NCOUNT ; i++) {
            count = (pkt->data[i * 4] << 8) +  pkt->data[(i * 4) + 1];
            // ignore count timestamp if count is zero
            if (count == 0) {
                period = 0.0;
                // accumulate time for <1 counts per sample
                pctx->tstamp[i] += sample_usec/1000000.0;
            }
            else {
                timestmp = (pkt->data[(i * 4) + 2] << 8) +  pkt->data[(i * 4) + 3];
                period = pctx->tstamp[i] + timestmp/1000000.0;  // in sec
                pctx->tstamp[i] = (sample_usec - timestmp) / 1000000.0;
            }
            ret = sprintf(&(cstr[clen]), "%4d %3.6f ", count, period);
            clen += ret;    // add to length of output string
            // length of "%4d %3.6f " is at most 16
            if ((ret < 0) || (clen > 16 * (i + 1))) {
                return;     // error in sprintf??
            }
        }
        (void) sprintf(&(cstr[clen]), "\n");  // terminate the output string
        clen++;                               // for the newline
        send_ui(cstr, clen, prsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(cstr, clen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * userparm():  - The user is reading or setting a configuration param
 **************************************************************/
static void userparm(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    COUNT4DEV *pctx;   // our local info
    int      ret;      // return count
    int      newrate;  // new value to assign the direction
    int      e1,e2,e3,e4; // new edge values (must be 0 to 3)
    uint8_t  ed;       // edges at 8 bit int

    pctx = (COUNT4DEV *) pslot->priv;

    if (rscid == RSC_RATE) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%d\n", (pctx->rate +1) * 10);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%d", &newrate);
            if ((ret != 1) || (newrate > 80) || (newrate < 10)) {
                ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                return;
            }

            // Force new rate to range of 10-80ms as number in range 0-7
            pctx->rate = (newrate / 10) - 1;
        }
    }
    if (rscid == RSC_EDGES) {
        if (cmd == EDGET) {
            ed = pctx->edges;
            ret = snprintf(buf, *plen, "%d %d %d %d\n", (ed & 0x03), ((ed % 0x0c) >> 2),
                           ((ed % 0x30) >> 4), ((ed % 0xc0) >> 6));
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            pctx->edges = 0xff;
            ret = sscanf(val, "%d %d %d %d", &e1, &e2, &e3, &e4);
            if ((ret != 4) ||
                (e1 < 0) || (e1 > 3) || (e2 < 0) || (e2 > 3) ||
                (e3 < 0) || (e3 > 3) || (e4 < 0) || (e4 > 3)) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->edges = (e4 << 6) | (e3 << 4) | (e2 << 2) | e1;
        }
    }
    sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send params to the FPGA card. 
 * Put error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    COUNT4DEV *pctx,   // This peripheral's context
    int      *plen,    // size of buf on input, #char in buf on output
    char     *buf)     // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the count4
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Write the values for sample rate and which edges to count
    // down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = COUNT4_REG_RATE;   // the first reg of the two
    pkt.count = 2;               // 2 data bytes
    pkt.data[0] = pctx->rate;    // Set the poll rate
    pkt.data[1] = pctx->edges;   // Set which edges to count

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
    COUNT4DEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of count4.c
