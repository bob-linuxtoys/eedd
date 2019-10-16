/*
 *  Name: roten.c
 *
 *  Description: Driver for the rotary encoder peripheral card which provides
 *        a simple user interface component consisting of a rotary encoder, a
 *        push button, and an LED.
 *
 *  Hardware Registers:
 *    0:  A read-only register with the low 7 bit containing  the number of
 *        quadrature encoder counts since the last time the register was read,
 *        and with the high bit containing the current state of the button.
 *        This register is usually sent automatically by the Baseboard.
 *    1:  The LED state in bit 0.  This is a read-write register.
 * 
 *  Resources:
 *    encoder      - space separated rotary count and button status
 *    led          - dpset to 0 or 1 to turn off or on the LED
 *
 * Copyright:   Copyright (C) 2015-2019 Demand Peripherals, Inc.
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
        // ROTEN register definitions
#define ROTEN_REG_COUNT    0x00
#define ROTEN_REG_LED      0x01
        // resource names and numbers
#define RSC_ENCODER        0
#define RSC_LED            1
        // Length of output line (e.g. "-63, 1\n"
#define MAX_LINE_LEN       20


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an roten
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      count;    // most recent reading of the rotation count
    int      button;   // most recent reading of the button state
    int      led;      // the state of the LED
} ROTENDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user(int, int, char*, SLOT*, int, int*, char*);
static int  tofpga(ROTENDEV *);
static void noAck(void *, ROTENDEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    ROTENDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (ROTENDEV *) malloc(sizeof(ROTENDEV));
    if (pctx == (ROTENDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in roten initialization");
        return (-1);
    }

    // Init our ROTENDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->count = 0;
    pctx->button = 0;          // we don't really know the button state yet
    pctx->led = 0;             // turn off LED at system start


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_ENCODER].name = "encoder";
    pslot->rsc[RSC_ENCODER].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_ENCODER].bkey = 0;
    pslot->rsc[RSC_ENCODER].pgscb = user;
    pslot->rsc[RSC_ENCODER].uilock = -1;
    pslot->rsc[RSC_ENCODER].slot = pslot;
    pslot->rsc[RSC_LED].name = "led";
    pslot->rsc[RSC_LED].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_LED].bkey = 0;
    pslot->rsc[RSC_LED].pgscb = user;
    pslot->rsc[RSC_LED].uilock = -1;
    pslot->rsc[RSC_LED].slot = pslot;
    pslot->name = "roten";
    pslot->desc = "General purpose rotary encoder input";
    pslot->help = README;

    // turn off the LED.  Ignore any errors
    (void) tofpga(pctx);
    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    ROTENDEV *pctx;    // our local info
    RSC      *prsc;    // pointer to this slot's encoder resource
    char      buf[MAX_LINE_LEN];
    int       nchar;

    pctx = (ROTENDEV *)(pslot->priv);  // Our "private" data is a ROTENDEV
    prsc = &(pslot->rsc[RSC_ENCODER]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the rotary counter should come in since we don't ever read
    // the LED value
    if ((pkt->reg != ROTEN_REG_COUNT) || (pkt->count != 1)) {
        edlog("invalid roten packet from board to host");
        return;
    }

    // This must be an autosend packet.  Record the values and
    // broadcast it if any UI are monitoring it.
    if (pkt->data[0] & 0x40)
        pctx->count = -((~(pkt->data[0]) + 1) & 0x3f);
    else
        pctx->count = pkt->data[0] & 0x3f;
    pctx->button = pkt->data[0] >> 7;

    if (prsc->bkey != 0) {
        nchar = sprintf(buf, "% 4d %d\n", pctx->count, pctx->button);
        send_ui(buf, nchar, prsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(buf, nchar, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * user():  - Handle setting the LED or getting the count and
 * button state using dpget.
 **************************************************************/
static void user(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    ROTENDEV *pctx;    // our local info
    int      ret;      // return count
    int      newled;   // new value of LED from user
    int      txret;    // ==0 if the packet went out OK

    pctx = (ROTENDEV *) pslot->priv;

    // Read of the most recent encoder values?
    if ((cmd == EDGET) && (rscid == RSC_ENCODER)) {
        ret = snprintf(buf, *plen, "% 4d, %d\n", pctx->count, pctx->button);
        *plen = ret;  // (errors are handled in calling routine)
    }
    // Read of the LED state?
    else if ((cmd == EDGET) && (rscid == RSC_LED)) {
        ret = snprintf(buf, *plen, "%d\n", pctx->led);
        *plen = ret;  // (errors are handled in calling routine)
    }
    // Write of the LED value?
    else if ((cmd == EDSET) && (rscid == RSC_LED)) {
        ret = sscanf(val, "%d", &newled);
        if ((ret != 1) || (newled < 0) || (newled > 1)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->led = newled;

        // Send new value to LED.  Report errors
        txret = tofpga(pctx);
        if (txret != 0) {
            // the send of the new pin values did not succeed.  This
            // probably means the input buffer to the USB port is full.
            // Tell the user of the problem.
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else {
            *plen = 0;    // no errors to report
        }
    }

    return;
}



/**************************************************************
 * tofpga():  Send config down to the board.
 **************************************************************/
static int tofpga(
    ROTENDEV *pctx)    // this instance of the rotary encoder
{
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    DP_PKT   pkt;      // send write and read cmds to the roten
    int      txret;    // ==0 if the packet went out OK

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = ROTEN_REG_LED; 
    pkt.count = 1;
    pkt.data[0] = pctx->led;
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    // Start timer to look for a write response.
    if ((txret == 0) && (pctx->ptimer == 0)) {
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
    }
    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    ROTENDEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of roten.c
