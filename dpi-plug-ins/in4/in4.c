/*
 *  Name: in4.c
 *
 *  Description: Driver for the quad input peripheral which uses four
 *        FPGA pins for input.  Each pin may be configured to send up
 *        to the host the new value of the input when the input changes.
 *
 *  Hardware Registers:
 *    0:  The current values at all four pins.  Bit 0 corresponds to the
 *        lowest numbered pin on the BaseBoard connector.
 *    1:  Interrupt-on-change configuration register. Here an "interrupt"
 *        refers to asynchronously sending to the host a USB packet that
 *        contains the most recent values available at the pins.  Setting
 *        a bit to one enables interrupt-on-change for the corresponding
 *        pin.  The power up default is to turn off interrupt-on-change.
 *        A change on an enabled interrupt pin causes the peripheral to
 *        send to the host a read response packet for a read of one 8-bit
 *        register starting at register 0.
 * 
 *  Resources:
 *    inputs    - values on all four pins as a hex digit (dpget/dpcat)
 *    interrupt - interrupt-on-change mask (dpset/dpget)
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
        // IN4 register definitions
#define IN4_REG_DATA        0x00
#define IN4_REG_INTERRUPT   0x01
        // max line length from user messages and input
#define MAX_LINE_LEN        100
        // Resource index numbers
#define RSC_INPUTS          0
#define RSC_INTERRUPT       1


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an in4
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      intrr;    // interrupt-on-change mask
} IN4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userinputs(int, int, char*, SLOT*, int, int*, char*);
static void userinterrupt(int, int, char*, SLOT*, int, int*, char*);
static int  tofpga(IN4DEV *);
static void noAck(void *, IN4DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    IN4DEV *pctx;      // our local device context

    // Allocate memory for this peripheral
    pctx = (IN4DEV *) malloc(sizeof(IN4DEV));
    if (pctx == (IN4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in in4 initialization");
        return (-1);
    }

    // Init our IN4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->intrr = 0;           // Matches Verilog default value

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add handlers for user visible resources
    pslot->rsc[RSC_INPUTS].name = "inputs";
    pslot->rsc[RSC_INPUTS].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_INPUTS].bkey = 0;
    pslot->rsc[RSC_INPUTS].pgscb = userinputs;
    pslot->rsc[RSC_INPUTS].uilock = -1;
    pslot->rsc[RSC_INPUTS].slot = pslot;
    pslot->rsc[RSC_INTERRUPT].name = "interrupt";
    pslot->rsc[RSC_INTERRUPT].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_INTERRUPT].bkey = 0;
    pslot->rsc[RSC_INTERRUPT].pgscb = userinterrupt;
    pslot->rsc[RSC_INTERRUPT].uilock = -1;
    pslot->rsc[RSC_INTERRUPT].slot = pslot;
    pslot->name = "in4";
    pslot->desc = "Quad input port";
    pslot->help = README;

    (void) tofpga(pctx);    // init the interrupt register

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
    IN4DEV *pctx;      // our local info
    RSC    *prsc;      // pointer to this slot's inputs resource
    char    buf[MAX_LINE_LEN];
    int     nchar;

    pctx = (IN4DEV *)(pslot->priv);  // Our "private" data is a IN4DEV
    prsc = &(pslot->rsc[RSC_INPUTS]);


    // The most likely packet is an autoupdate or a read response.
    // Reply and broadcast update if any UI are monitoring it.
    if (((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_READ) &&
        (pkt->reg == IN4_REG_DATA) && (pkt->count == 1)) {

        // If a read response from a user dpget command, send value to UI
        if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) {
            nchar = sprintf(buf, "%1x\n", pkt->data[0] & 0x0f);
            send_ui(buf, nchar, prsc->uilock);
            prompt(prsc->uilock);
            // Response sent so clear the lock
            prsc->uilock = -1;
            del_timer(pctx->ptimer);  //Got the response
            pctx->ptimer = 0;
            return;
        }
        else {
            // not response, must be an autosend
            nchar = sprintf(buf, "%1x\n", (pkt->data[0] & 0x0f));
            send_ui(buf, nchar, prsc->uilock);
            if (prsc->bkey != 0) {
                // bkey will return cleared if UIs are no longer monitoring us
                bcst_ui(buf, nchar, &(prsc->bkey));
            }
            return;
        }
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
    if ((pkt->reg != IN4_REG_DATA) || (pkt->count != 1)) {
        edlog("invalid in4 packet from board to host");
    }

    return;
}


/**************************************************************
 * userinputs():  - The user is reading the current state of 
 * the pins.  Send a read request to the FPGA card then record
 * the UI id number so the packet handler knows where to send
 * the reply.
 **************************************************************/
static void userinputs(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    IN4DEV *pctx;    // our local info
    DP_PKT   pkt;      // packet to the FPGA card
    CORE    *pmycore;  // FPGA peripheral info
    int      ret;      // return count
    int      txret;    // send status

    // nothing to do if invoked to set up a dpcat
    if (cmd == EDCAT) {
        // Nothing to send back to the user
        *plen = 0;
        return;
    }

    pctx = (IN4DEV *) pslot->priv;
    pmycore = pslot->pcore;

    // create a read packet to get the current value of the pins
    pkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = IN4_REG_DATA;
    pkt.count = 1;

    // send the packet.  Report any errors
    txret = dpi_tx_pkt(pmycore, &pkt, 4); // 4 header + 0 data for read
    if (txret != 0) {
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Start timer to look for a read response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);

    // lock this resource to the UI session cn
    pslot->rsc[RSC_INPUTS].uilock = (char) cn;

    // Nothing to send back to the user
    *plen = 0;

    return;
}


/**************************************************************
 * userinterrupt():  - The user is reading or writing the config.
 * Get the value and update the  BaseBoard or read the value and
 * write it into the supplied buffer.
 **************************************************************/
static void userinterrupt(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    IN4DEV *pctx;      // our local info
    int      ret;      // return count
    int      newintrr; // new value to assign the leds
    int      txret;    // ==0 if the packet went out OK


    pctx = (IN4DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%1x\n", pctx->intrr);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // User is updating the interrupt-on-change register
    ret = sscanf(val, "%x", &newintrr);
    if ((ret != 1) || (newintrr < 0) || (newintrr > 0xf)) {
        ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
        *plen = ret;
        return;
    }
    pctx->intrr = newintrr;

    // Got a new value for the config.  Send down to the card.
    txret = tofpga(pctx);
    if (txret != 0) {
        // the send of the new config did not succeed.  This
        // probably means the input buffer to the USB port is full.
        // Tell the user of the problem.
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

 
    return;
}


/**************************************************************
 * tofpga():  Send config down to the FPGA 
 **************************************************************/
int tofpga(
    IN4DEV *pctx)      // Send config to this peripheral
{
    int      txret;    // ==0 if the packet went out OK
    DP_PKT   pkt;      // send write and read cmds to the in4
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // Build and send the write command to set the interrupt config
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = IN4_REG_INTERRUPT;
    pkt.count = 1;
    pkt.data[0] = pctx->intrr;
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + 1 data

    // Start timer to look for a write response.
    if ((txret ==0) && (pctx->ptimer == 0)) {
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
    IN4DEV *pctx)    // No response from this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}
// end of in4.c
