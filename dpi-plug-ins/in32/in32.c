/*
 *  Name: in32.c
 *
 *  Description: Driver for the 32 channel input peripheral
 *
 *      The 32 channel input card provides 32 input pins that
 *  are 5 volt tolerant. Each pin may be configured to send 
 *  up to the host the new value of the input when the input
 *  changes.
 *
 *  Hardware Registers:
 *      Reg 0:  Bit 0 is the value at pin 8 input and is read-only.
 *              Bit 1 is set to enable interrupt on change and is read-write
 *      Reg 1:  As above for pin 7
 *      Reg 2:  As above for pin 6
 *           :                   :
 *      Reg 27: As above for pin 28
 *      Reg 28: As above for pin 29
 *      Reg 29: As above for pin 30
 *      Reg 30: As above for pin 31
 *      Reg 31: As above for pin 32
 * 
 *  Resources:
 *    input      - 32 bit hex value at the inputs.  Works with dpget and dpcat
 *    interrupt  - interrupt (autosend) on change, 0==poll, 1==autosend
 *
 *  Copyright:   Copyright (C) 2018-2019 Demand Peripherals, Inc.
 *               All rights reserved.
 *
 *  License:     This program is free software; you can redistribute it and/or
 *               modify it under the terms of the Version 2 of the GNU General
 *               Public License as published by the Free Software Foundation.
 *               GPL2.txt in the top level directory is a copy of this license.
 *               This program is distributed in the hope that it will be useful,
 *               but WITHOUT ANY WARRANTY; without even the implied warranty of
 *               MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *               GNU General Public License for more details. 
 *
 *               Please contact Demand Peripherals if you wish to use this code
 *               in a non-GPLv2 compliant manner. 
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
        // in32 register definitions. Just need the first. All the same
        // Note that on the board the pins are labeled 1 to 8, not 0 to 7
#define IN32_R_PIN0        0x00
        // Mask for in/intr/out for pin descriptions
#define IN32_M_INPUT       0x01
#define IN32_M_INTR        0x02
        // resource names and numbers
#define FN_INPUT           "input"
#define FN_INTR            "interrupt"
#define RSC_INPUT          0
#define RSC_INTR           1
        // Number of pin on this card
#define NPINS              32


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an in32
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    unsigned int intr; // interrupt on change setting for inputs
} in32DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user_hdlr(int, int, char*, SLOT*, int, int*, char*);
static void sendconfigtofpga(in32DEV *, int *plen, char *buf);
static void noAck(void *, in32DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    in32DEV *pctx;      // our local device context

    // Allocate memory for this peripheral
    pctx = (in32DEV *) malloc(sizeof(in32DEV));
    if (pctx == (in32DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in in32 initialization");
        return (-1);
    }

    // Init our in32DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->intr = 0;            // no interrupt-on-change to start

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_INPUT].name = FN_INPUT;
    pslot->rsc[RSC_INPUT].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_INPUT].bkey = 0;
    pslot->rsc[RSC_INPUT].pgscb = user_hdlr;
    pslot->rsc[RSC_INPUT].uilock = -1;
    pslot->rsc[RSC_INPUT].slot = pslot;
    pslot->rsc[RSC_INTR].name = FN_INTR;
    pslot->rsc[RSC_INTR].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_INTR].bkey = 0;
    pslot->rsc[RSC_INTR].pgscb = user_hdlr;
    pslot->rsc[RSC_INTR].uilock = -1;
    pslot->rsc[RSC_INTR].slot = pslot;
    pslot->name = "in32";
    pslot->desc = "Thirty-two channel input";
    pslot->help = README;

    // Send the output and interrupt setting to the card.
    // Ignore return value since there's no user connection and
    // system errors are sent to the logger.
    sendconfigtofpga(pctx, (int *) 0, (char *) 0);  // send pins, dir, intr

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
    in32DEV *pctx;     // context of this in32 instance
    RSC    *prsc;      // pointer to this slot's input resource
    int     i;         // generic loop counter
    unsigned int inval; // value of the inputs
    int     inlen;     // length of pin values string (should be 3) 
    char    instr[12];  // pin values as a 8 digit hex string

    pctx = (in32DEV *)(pslot->priv);  // Our "private" data is a in32DEV
    prsc = &(pslot->rsc[RSC_INPUT]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the pins should come in since we don't ever read the output
    // or interrupt registers.
    if ((pkt->reg != IN32_R_PIN0) || (pkt->count != NPINS)) {
        edlog("invalid in32 packet from board to host");
        return;
    }

    // Get the hex value of the 32 input pins
    inval = 0;
    for (i = 0; i < NPINS; i++) {    // pull input values from bit0
       inval = inval | (pkt->data[NPINS - 1 - i] & IN32_M_INPUT) << i; 
    }
    inlen = sprintf(instr, "%08x\n", inval);

    // If a read response from a user dpget command, send value to UI
    if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) {
        send_ui(instr, inlen, prsc->uilock);
        prompt(prsc->uilock);
        // Response sent so clear the lock
        prsc->uilock = -1;
        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;
        return;
    }

    // Process of elimination makes this an autosend packet.
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey != 0) {
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(instr, inlen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * user_hdlr():  - The user is reading or setting a resource
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
    in32DEV  *pctx;    // context for this peripheral instance
    DP_PKT   pkt;      // send write and read cmds to the in32
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // return count
    unsigned int tmp;  // temporary int to help conversion

    pctx = (in32DEV *) pslot->priv;
    pmycore = pslot->pcore;

    // Possible UI is for input or interrupt
    if (rscid == RSC_INTR) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%08x\n", pctx->intr);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%x", &tmp);
            if (ret != 1) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->intr = tmp;
            sendconfigtofpga(pctx, plen, buf);  // send output and intr config to FPGA
            return;
        }
    }
    else {          // must be dpset or dpcat for inputs
        if (cmd == EDGET) {
            // create a read packet to get the current value of the pins
            pkt.cmd = DP_CMD_OP_READ | DP_CMD_AUTOINC;
            pkt.core = pmycore->core_id;
            pkt.reg = IN32_R_PIN0;
            pkt.count = NPINS;
            // send the packet.  Report any errors
            txret = dpi_tx_pkt(pmycore, &pkt, 4);   // 4 header + 0 data for read
            if (txret != 0) {
                ret = snprintf(buf, *plen, E_WRFPGA);
                *plen = ret;  // (errors are handled in calling routine)
                return;
            }
            // Start timer to look for a read response.
            if (pctx->ptimer == 0)
                pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
            // lock this resource to the UI session cn
            pslot->rsc[RSC_INPUT].uilock = cn;
            // Nothing to send back to the user
            *plen = 0;
        }
        // else = nothing.  No action needed for dpcat of inputs
    }

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send pin values to the FPGA card.  Put
 * any error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    in32DEV  *pctx,     // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the in32
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value
    int      i;        // generic loop counter
    unsigned int mask; // shift mast to test for set bits

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Send values for the interrupt mask down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = IN32_R_PIN0;   // pin value/config consecutive from pin0
    pkt.count = NPINS;
    mask = 0x80000000;
    for (i = 0; i < NPINS; i++) {
        pkt.data[i] = (pctx->intr & mask) ? IN32_M_INTR : 0;
        mask = mask >> 1;
    }
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
    void   *timer,   // handle of the timer that expired
    in32DEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of in32.c
