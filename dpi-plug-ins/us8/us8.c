/*
 *  Name: us8.c
 *
 *      The us8 peripheral uses a us8 card to connect to up
 *  to eight SRF-04 ultrasonic sensors.  The sensors have one
 *  input pin and one output pin.  A 10 us pulse on the input
 *  starts a ping.  The echo time of the ping is given as a
 *  pulse width on the output pin.  The echo response starts
 *  about 100 us after the end of the start pulse.  The FPGA
 *  measures the pulse width of the echo reply and does an
 *  auto-send of that time up to the host.  To avoid multiple
 *  echoes the peripheral pings only one sensor at a time with
 *  pings sent every 60 millisecond.
 *      The user can select which sensors are enabled.  The scan
 *  rate per sensor is faster if you disable unused sensors.

 *  Hardware Registers:
 *      Reg 0:  read-only, low byte of 12 bit timer
 *      Reg 1:  read-only, sensor ID and upper 4 bits of timer
 *      Reg 2:  enable register
 *
 *      Every ping results in the sending on one sample.  Both
 *  register 1 and 2 are sent as part of a sample.  
 *
 *
 *  Resources:
 *    distance   - sensor ID and its distance (works with dpcat)
 *    enable     - bit mask to enable (1) or disable (0) a sensor (dpset/dpget)
 *
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
        // US8 register definitions
#define US8_R_TIME         0      // ping time low 8 bits
#define US8_R_ID           1      // sensor ID (1-8) and upper 4 bits of time
#define US8_R_ENABLE       2      // enabled sensors (bits 0-8)
        // resource names and numbers
#define RSC_DISTANCE       0
#define RSC_ENABLE         1
        // Length of output line (e.g. "63, 1\n"
#define MAX_LINE_LEN       20
        // Raw time values from the sensor are in units of 10 us
#define TIME_TO_DIST(t) ((74006 * (t)) / 100000) // 7.4 uSec/tenth-inch
        // distance reported by a failed or missing sensor
#define NOSENSORDIST       3030


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of a us8
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      enable;   // bit mask to indicate if a sensor is enabled
} US8DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user_hdlr(int, int, char*, SLOT*, int, int*, char*);
static int  sendconfigtofpga(US8DEV *);
static void noAck(void *, US8DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    US8DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (US8DEV *) malloc(sizeof(US8DEV));
    if (pctx == (US8DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in us8 initialization");
        return (-1);
    }

    // Init our US8DEV structure
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
    pslot->rsc[RSC_DISTANCE].pgscb = user_hdlr;
    pslot->rsc[RSC_DISTANCE].uilock = -1;
    pslot->rsc[RSC_DISTANCE].slot = pslot;
    pslot->rsc[RSC_ENABLE].name = "enable";
    pslot->rsc[RSC_ENABLE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_ENABLE].bkey = 0;
    pslot->rsc[RSC_ENABLE].pgscb = user_hdlr;
    pslot->rsc[RSC_ENABLE].uilock = -1;
    pslot->rsc[RSC_ENABLE].slot = pslot;
    pslot->name = "us8";
    pslot->desc = "Octal interface to SRF04 distance sensor";
    pslot->help = README;

    // Turn off the sensors.  Ignore any errors
    (void) sendconfigtofpga(pctx);

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
    US8DEV *pctx;      // our local info
    RSC    *prsc;      // pointer to this slot's encoder resource
    char    buf[MAX_LINE_LEN];
    int     nchar;
    int     dist;

    pctx = (US8DEV *)(pslot->priv);  // Our "private" data is a US8DEV
    prsc = &(pslot->rsc[RSC_DISTANCE]);

    // The most likely packet is an autoupdate.  Check for it first
    // and broadcast update if any UI are monitoring it.
    if (((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
        ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_READ) &&
        (pkt->reg == US8_R_TIME) &&
        (pkt->count == 3) &&
        (prsc->bkey != 0)) {
        dist = TIME_TO_DIST(((pkt->data[1] & 0x0f) << 8) + pkt->data[0]);
        // Zero the distance if at 12 bit max distance
        nchar = snprintf(buf, MAX_LINE_LEN, "%d %d\n", ((pkt->data[1] & 0xf0) >> 4), dist);
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
    if ((pkt->reg != US8_R_TIME) || (pkt->count != 3)) {
        edlog("invalid us8 packet from board to host");
    }

    return;
}


/**************************************************************
 * user_hdlr():  - Handle reading or writing the enable register
 **************************************************************/
static void user_hdlr(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    US8DEV  *pctx;     // our local info
    int      ret;      // return count
    int      nable;    // new value of enable mask from user
    int      txret;    // ==0 if the packet went out OK

    pctx = (US8DEV *) pslot->priv;

    // Read of the enabled state?
    if ((cmd == EDGET) && (rscid == RSC_ENABLE)) {
        ret = snprintf(buf, *plen, "%02x\n", pctx->enable);
        *plen = ret;  // (errors are handled in calling routine)
    }
    // Write of the enable value?
    else if ((cmd == EDSET) && (rscid == RSC_ENABLE)) {
        ret = sscanf(val, "%x", &nable);
        if ((ret != 1) || (nable < 0) || (nable > 0xff)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->enable = nable;

        // Send new value to the FPGA.  Report errors
        txret = sendconfigtofpga(pctx);
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
 * sendconfigtofpga():  Send config down to the board.
 **************************************************************/
static int sendconfigtofpga(
    US8DEV *pctx)    // our device instance
{
    SLOT    *pslot;    // This peripheral's slot info
    DP_PKT   pkt;      // send write and read cmds to the us8
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = US8_R_ENABLE; 
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
    void   *timer,   // handle of the timer that expired
    US8DEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of us8.c
