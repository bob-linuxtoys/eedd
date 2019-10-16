/*
 *  Name: touch4.c
 *
 *  Description: Quad touch sensor input
 *
 *  The FPGA peripheral is simply that of a quad counter (count4).
 *  The code in this driver interpretes the counts to determine a
 *  touch event.
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
 *    threshold   - percent drop in frequency to be considered a "touch"
 *    counts      - raw frequencies at the inputs
 *    touch       - touch sensor status as a single hex digit
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
#define FN_TOUCH           "touch"
#define FN_THRESHOLDS      "thresholds"
#define RSC_COUNTS         0
#define RSC_TOUCH          1
#define RSC_THRESHOLDS     2

        // default parameters
#define DEF_THRESHOLD      10   /* default threshold as a percent */
#define IIR_RATE           960  /* about a minute of samples */


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an touch4
typedef struct
{
    void    *pslot;       // handle to peripheral's slot info
    void    *ptimer;      // timer to watch for dropped ACK packets
    uint8_t  rate;        // sample rate
    uint8_t  edges;       // which edges to detect
    int      firstsample; // set while waiting for inital readings to settle
    int      laststatus;  // used for edge detection on output of touch
    uint64_t averag[NCOUNT]; // IIR running average
    uint32_t thresh[NCOUNT]; // threshold as a percentage
} TOUCH4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userparm(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, TOUCH4DEV *);
static void tofpga(TOUCH4DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)          // points to the SLOT for this peripheral
{
    TOUCH4DEV *pctx;      // our local device context
    int        i;

    // Allocate memory for this peripheral
    pctx = (TOUCH4DEV *) malloc(sizeof(TOUCH4DEV));
    if (pctx == (TOUCH4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in touch4 initialization");
        return (-1);
    }

    // Init our TOUCH4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->rate = 5;            // Poll interval in units of 10ms where 0=10ms and 5=60ms
    pctx->edges = 0xff;        // count both positive and negative edges
    pctx->firstsample = 3;     // sample period may be off on first few samples
    pctx->laststatus = 0;      // assume no input active at start
    for (i = 0; i < NCOUNT; i++) {
        pctx->averag[i] = 0;
        pctx->thresh[i] = DEF_THRESHOLD;
    }


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
    pslot->rsc[RSC_TOUCH].name = FN_TOUCH;
    pslot->rsc[RSC_TOUCH].flags = CAN_BROADCAST;
    pslot->rsc[RSC_TOUCH].bkey = 0;
    pslot->rsc[RSC_TOUCH].pgscb = 0;
    pslot->rsc[RSC_TOUCH].uilock = -1;
    pslot->rsc[RSC_TOUCH].slot = pslot;
    pslot->rsc[RSC_THRESHOLDS].name = FN_THRESHOLDS;
    pslot->rsc[RSC_THRESHOLDS].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_THRESHOLDS].bkey = 0;
    pslot->rsc[RSC_THRESHOLDS].pgscb = userparm;
    pslot->rsc[RSC_THRESHOLDS].uilock = -1;
    pslot->rsc[RSC_THRESHOLDS].slot = pslot;
    pslot->name = "touch4";
    pslot->desc = "Quad touch input";
    pslot->help = README;


    tofpga(pctx);

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
    TOUCH4DEV *pctx;    // our local info
    RSC       *pcountrsc;  // pointer to count resource
    RSC       *ptouchrsc;  // pointer to touch resource
    int        raw[NCOUNT]; // raw counts from the count4 peripheral
    char       cstr[100];  // space for four ints or a single hex char
    int        status;  // touch status as a single hex character
    int        ret;     // return value from sprintf to cstr
    int        i;       // counter under consideration
    int        clen;    // length of count output string


    pctx = (TOUCH4DEV *)(pslot->priv);  // Our "private" data is a GPIO4DEV
    // ignore very first sample since the count4 sample period might
    // not be correct on the first sample
    if (pctx->firstsample) {
        pctx->firstsample--;
        return;
    }

    pcountrsc = &(pslot->rsc[RSC_COUNTS]);
    ptouchrsc = &(pslot->rsc[RSC_TOUCH]);

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
        edlog("invalid touch4 packet from board to host");
        return;
    }

    // Process of elimination makes this an autosend packet.  Get counts
    for (i = 0; i < NCOUNT ; i++) {
        raw[i] = (pkt->data[i * 4] << 8) +  pkt->data[(i * 4) + 1];
    }

    // Broadcast raw counts if any UI is monitoring it.
    if (pcountrsc->bkey != 0) {
        clen = 0;
        for (i = 0; i < NCOUNT ; i++) {
            ret = sprintf(&(cstr[clen]), "%4d ", raw[i]);
            clen += ret;    // add to length of output string
        }
        (void) sprintf(&(cstr[clen]), "\n");  // terminate the output string
        clen++;                               // for the newline
        send_ui(cstr, clen, pcountrsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(cstr, clen, &(pcountrsc->bkey));
    }

    // Compute and broadcast sensor status if any UI is monitoring it
    if (ptouchrsc->bkey != 0) {
        status = 0;
        for (i = 0; i < NCOUNT ; i++) {
            if (raw[i] < (pctx->averag[i] / IIR_RATE) * (100 - pctx->thresh[i]) / 100)
                status += 1 << i;
        }
        if (status != pctx->laststatus) {
            clen =  sprintf(cstr, "%x\n", status);  // send one hex char
            send_ui(cstr, clen, ptouchrsc->uilock);
            // bkey will return cleared if UIs are no longer monitoring us
            bcst_ui(cstr, clen, &(ptouchrsc->bkey));
            pctx->laststatus = status;
        }
    }

    // Add raw counts to average or, if zero, init average
    // averag is 32 bits, iir_rate is about 10 and count is 16
    for (i = 0; i < NCOUNT ; i++) {
        if (pctx->averag[i] == 0)
            pctx->averag[i] = IIR_RATE * raw[i];
        else
            pctx->averag[i] = raw[i] + (pctx->averag[i] * (IIR_RATE - 1) / IIR_RATE);
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
    TOUCH4DEV *pctx;   // our local info
    int      ret;      // return count
    int      t1,t2,t3,t4; // new thresholds

    pctx = (TOUCH4DEV *) pslot->priv;

    if (rscid == RSC_THRESHOLDS) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%d %d %d %d\n", pctx->thresh[0],
                          pctx->thresh[1], pctx->thresh[2], pctx->thresh[3]);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            pctx->edges = 0xff;
            ret = sscanf(val, "%d %d %d %d", &t1, &t2, &t3, &t4);
            if ((ret != 4) ||
                (t1 < 0) || (t1 > 100) || (t2 < 0) || (t2 > 100) ||
                (t3 < 0) || (t3 > 100) || (t4 < 0) || (t4 > 100)) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->thresh[0] = t1;
            pctx->thresh[1] = t2;
            pctx->thresh[2] = t3;
            pctx->thresh[3] = t4;
        }
    }

    return;
}


/**************************************************************
 * tofpga():  - Send config values to the FPGA card.
 **************************************************************/
static void tofpga(
    TOUCH4DEV *pctx)   // This peripheral's context
{
    DP_PKT   pkt;      // send write and read cmds to the adc812
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

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

    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header +  2 data

    // Start timer to look for a write response.
    if ((txret == 0) && (pctx->ptimer == 0)) {
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
    }

    return;
}




/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    TOUCH4DEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of touch4.c
