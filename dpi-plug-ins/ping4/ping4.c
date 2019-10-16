/*
 *  Name: ping4.c
 *
 *    The quad Parallax Ping interface makes each pin on the connector
 *  into a channel of bidirectional I/O that connects directly to a
 *  Parallax Ping ultrasonic range sensor.  The first channel appears
 *  on the BaseBoard connector pin 2 or on pin 10 depending on the slot
 *  number for the peripheral.  The fourth channel is on pin 8 or 16.
 *  The ultrasonic pings are sent out sequentially from each sensor so
 *  that the pings do not interfere with each other.
 *
 *  Hardware Registers:
 *    The Quad Parallax Ultrasonic Range Finder uses four 8-bit registers
 *  for control and status.
 *
 *    0/1: Echo time in microseconds with 15 bits of resolution.  There
 *         are 7.3746 uSec/tenth-inch or .1356 tenth-inches per uSec.
 *    2:  Interface number (1-4).
 *    3:  Enable register.  Bits 0-3 are a bit mask where 1 means enabled.
 *
 * 
 *  Resources:
 *    distance   - sensor ID and its distance (works with dpcat)
 *    enable     - bit mask to enable (1) or disable (0) a sensor (dpset/dpget)
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
        // PING4 register definitions
#define PING4_REG_TIME     0      // ping time (14-bits resolution)
#define PING4_REG_ID       2      // sensor ID (1-4)
#define PING4_REG_ENABLE   3      // enabled sensors (bits 0-3)
        // resource names and numbers
#define RSC_DISTANCE       0
#define RSC_ENABLE         1
        // Length of output line (e.g. "-63, 1\n"
#define MAX_LINE_LEN       20
        // conversion factor
#define TIME_TO_DIST(t) ((74006 * (t)) / 1000000) // 7.4 uSec/tenth-inch


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of a ping4
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      enable;   // bit mask to indicate if a sensor is enabled
} PING4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user(int, int, char*, SLOT*, int, int*, char*);
static int  tofpga(PING4DEV *);
static void noAck(void *, PING4DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    PING4DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (PING4DEV *) malloc(sizeof(PING4DEV));
    if (pctx == (PING4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in ping4 initialization");
        return (-1);
    }

    // Init our PING4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->enable = 0;          // default is all off


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_DISTANCE].name = "distance";
    pslot->rsc[RSC_DISTANCE].flags = CAN_BROADCAST;
    pslot->rsc[RSC_DISTANCE].bkey = 0;
    pslot->rsc[RSC_DISTANCE].pgscb = user;
    pslot->rsc[RSC_DISTANCE].uilock = -1;
    pslot->rsc[RSC_DISTANCE].slot = pslot;
    pslot->rsc[RSC_ENABLE].name = "enable";
    pslot->rsc[RSC_ENABLE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_ENABLE].bkey = 0;
    pslot->rsc[RSC_ENABLE].pgscb = user;
    pslot->rsc[RSC_ENABLE].uilock = -1;
    pslot->rsc[RSC_ENABLE].slot = pslot;
    pslot->name = "ping4";
    pslot->desc = "Quad interface to a Parallax Ping)))";
    pslot->help = README;

    // Turn off the sensors.  Ignore any errors
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
    PING4DEV *pctx;    // our local info
    RSC      *prsc;    // pointer to this slot's encoder resource
    char      buf[MAX_LINE_LEN];
    int       nchar;
    int       dist;

    pctx = (PING4DEV *)(pslot->priv);  // Our "private" data is a PING4DEV
    prsc = &(pslot->rsc[RSC_DISTANCE]);


    // The most likely packet is an autoupdate.  Check for it first
    // and broadcast update if any UI are monitoring it.
    if (((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
        ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_READ) &&
        (pkt->reg == PING4_REG_TIME) &&
        (pkt->count == 3) &&
        (prsc->bkey != 0)) {
        dist = TIME_TO_DIST((pkt->data[0] << 8) + pkt->data[1]);
        nchar = snprintf(buf, MAX_LINE_LEN, "%d %d\n", pkt->data[2], dist);
        send_ui(buf, nchar, prsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(buf, nchar, &(prsc->bkey));
        return;
    }

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Sanity check: error if none of the above
    if ((pkt->reg != PING4_REG_TIME) || (pkt->count != 3)) {
        edlog("invalid ping4 packet from board to host");
    }

    return;
}


/**************************************************************
 * user():  - Handle reading or writing the enable register
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
    PING4DEV *pctx;    // our local info
    int      ret;      // return count
    int      nable;    // new value of enable mask from user
    int      txret;    // ==0 if the packet went out OK

    pctx = (PING4DEV *) pslot->priv;

    // Read of the enabled state?
    if ((cmd == EDGET) && (rscid == RSC_ENABLE)) {
        ret = snprintf(buf, *plen, "%d\n", pctx->enable);
        *plen = ret;  // (errors are handled in calling routine)
    }
    // Write of the enable value?
    else if ((cmd == EDSET) && (rscid == RSC_ENABLE)) {
        ret = sscanf(val, "%x", &nable);
        if ((ret != 1) || (nable < 0) || (nable > 15)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->enable = nable;

        // Send new value to the FPGA.  Report errors
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
    PING4DEV *pctx)    // our device instance
{
    SLOT    *pslot;    // This peripheral's slot info
    DP_PKT   pkt;      // send write and read cmds to the ping4
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = PING4_REG_ENABLE; 
    pkt.count = 1;
    pkt.data[0] = pctx->enable;
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
    PING4DEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of ping4.c
