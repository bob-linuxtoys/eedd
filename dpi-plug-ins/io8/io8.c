/*
 *  Name: io8.c
 *
 *  Description: Driver for the octal input/output peripheral
 *
 *      The octal I/O Port provides eight pins of output and eight pins of
 *  input.  Each pin may additionally be configured to send up to the host
 *  the new value of the input when the input changes.
 *
 *  Hardware Registers:
 *      Reg 0:  Bit 0 is the value at pin 8 input and is read-only.
 *              Bit 1 is set to enable interrupt on change and is read-write
 *              Bit 2 is the data out value and is read-write
 *      Reg 1:  As above for pin 7
 *      Reg 2:  As above for pin 6
 *      Reg 3:  As above for pin 5
 *      Reg 4:  As above for pin 4
 *      Reg 5:  As above for pin 3
 *      Reg 6:  As above for pin 2
 *      Reg 7:  As above for pin 1
 * 
 *  Resources:
 *    output       - 8 bit hex value of the outputs. Works with dpget and dpset
 *    input        - 8 bit hex value at the inputs.  Works with dpget and dpcat
 *    interrupt    - interrupt (autosend) on change, 0==poll, 1==autosend
 *
 *  Copyright:   Copyright (C) 2015-2019 Demand Peripherals, Inc.
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
        // IO8 register definitions. Just need the first. All the same
        // Note that on the board the pins are labeled 1 to 8, not 0 to 7
#define IO8_R_PIN0         0x00
        // Mask for in/intr/out for pin descriptions
#define IO8_M_INPUT        0x01
#define IO8_M_INTR         0x02
#define IO8_M_OUTPUT       0x04
        // resource names and numbers
#define FN_OUTPUT          "output"
#define FN_INPUT           "input"
#define FN_INTR            "interrupt"
#define RSC_OUTPUT         0
#define RSC_INPUT          1
#define RSC_INTR           2
        // The 8 in IO8 is the number of pins
#define NPINS              8


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an io8
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      outpins;  // value of the output pins
    int      intr;     // interrupt on change setting for inputs
} IO8DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user_hdlr(int, int, char*, SLOT*, int, int*, char*);
static void sendconfigtofpga(IO8DEV *, int *plen, char *buf);
static void noAck(void *, IO8DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    IO8DEV *pctx;      // our local device context

    // Allocate memory for this peripheral
    pctx = (IO8DEV *) malloc(sizeof(IO8DEV));
    if (pctx == (IO8DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in io8 initialization");
        return (-1);
    }

    // Init our IO8DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->outpins = 0;         // init with outputs set to zero
    pctx->intr = 0;            // no interrupt-on-change to start

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_OUTPUT].name = FN_OUTPUT;
    pslot->rsc[RSC_OUTPUT].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_OUTPUT].bkey = 0;
    pslot->rsc[RSC_OUTPUT].pgscb = user_hdlr;
    pslot->rsc[RSC_OUTPUT].uilock = -1;
    pslot->rsc[RSC_OUTPUT].slot = pslot;
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
    pslot->name = "io8";
    pslot->desc = "Octal Input / Octal Output";
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
    IO8DEV *pctx;      // context of this IO8 instance
    RSC    *prsc;      // pointer to this slot's input resource
    int     i;         // generic loop counter
    int     inval;     // value of the inputs
    int     inlen;     // length of pin values string (should be 3) 
    char    instr[10];  // pin values as a two digit hex string

    pctx = (IO8DEV *)(pslot->priv);  // Our "private" data is a IO8DEV
    prsc = &((pslot)->rsc[RSC_INPUT]);


    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the pins should come in since we don't ever read the output
    // or interrupt registers.
    if ((pkt->reg != IO8_R_PIN0) || (pkt->count != 8)) {
        edlog("invalid io8 packet from board to host");
        return;
    }

    // Get the hex value of the 8 input pins
    inval = 0;
    for (i = 0; i < NPINS; i++) {    // pull input values from bit0
       inval = inval | ((pkt->data[i] & IO8_M_INPUT) << i); 
    }
    inlen = sprintf(instr, "%02x\n", inval);

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
    IO8DEV  *pctx;     // context for this peripheral instance
    CORE    *pmycore;  // FPGA peripheral info
    DP_PKT   pkt;      // send write and read cmds to the io8
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // return count
    int      tmp;      // temporary int to help conversion

    pctx = (IO8DEV *) pslot->priv;
    pmycore = pslot->pcore;

    // Possible UI is for input, output, or interrupt
    if (rscid == RSC_OUTPUT) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%02x\n", pctx->outpins);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%x", &tmp);
            if ((ret != 1) || (tmp > 0xff)) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->outpins = tmp;
            sendconfigtofpga(pctx, plen, buf);  // send output and intr config to FPGA
            return;
        }
    }
    else if (rscid == RSC_INTR) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%02x\n", pctx->intr);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%x", &tmp);
            if ((ret != 1) || (tmp > 0xff)) {
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
            pkt.core = (pslot->pcore)->core_id;
            pkt.reg = IO8_R_PIN0;
            pkt.count = 8;
            // send the packet.  Report any errors
            txret = dpi_tx_pkt(pmycore, &pkt, 4);
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
    IO8DEV  *pctx,     // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the io8
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value
    int      i;        // generic loop counter
    int      mask;     // shift mast to test for set bits

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Send values for the outputs and interrupt mask down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = (pslot->pcore)->core_id;
    pkt.reg = IO8_R_PIN0;   // pin value/config consecutive from pin0
    pkt.count = 8;
    mask = 0x80;
    for (i = 0; i < NPINS; i++) {
        pkt.data[i] = (pctx->outpins & mask) ? IO8_M_OUTPUT : 0;
        pkt.data[i] |= (pctx->intr & mask) ? IO8_M_INTR : 0;
        mask = mask >> 1;
    }
    txret = dpi_tx_pkt(pmycore, &pkt, 12); // 4 header + 8 data

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
    IO8DEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of io8.c
