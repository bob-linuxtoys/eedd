/*
 *  Name: bb4io.c
 *
 *  Description: Driver for the buttons and LEDs on the FPGA card
 *
 *  Hardware Registers:
 *    0: buttons   - 8-bit read only
 *    1: LEDs      - 8-bit read/write
 * 
 *  Resources:
 *    leds         - read/write ASCII data to LEDs
 *    buttons      - broadcast ASCII auto-data from buttons
 *
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
        // BB4IO register definitions
#define BB4IO_REG_BUTTONS   0x00
#define BB4IO_REG_LEDS      0x01
        // line length from user to set LEDs value ( e.g. "0x55\n")
#define LEDVAL_LEN          10
#define FN_LEDS             "leds"
#define FN_BUTTONS          "buttons"
        // Resource index numbers
#define RSC_BUTTONS         0
#define RSC_LEDS            1


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an bb4io
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    unsigned char ledval; // Current value of the LEDs
    unsigned char swchval; // Most recent value of the switches
    void    *ptimer;   // timer to watch for dropped ACK packets
} BB4IODEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, BB4IODEV *);
static int  ledstofpga(BB4IODEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    BB4IODEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (BB4IODEV *) malloc(sizeof(BB4IODEV));
    if (pctx == (BB4IODEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in bb4io initialization");
        return (-1);
    }

    // Init our BB4IODEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->swchval = -1;        // Forces a post on first button update
    pctx->ledval = 0;          // Matches Verilog default value
    pctx->ptimer = 0;          // set while waiting for a response

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_BUTTONS].name = FN_BUTTONS;
    pslot->rsc[RSC_BUTTONS].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_BUTTONS].bkey = 0;
    pslot->rsc[RSC_BUTTONS].pgscb = usercmd;
    pslot->rsc[RSC_BUTTONS].uilock = -1;
    pslot->rsc[RSC_BUTTONS].slot = pslot;
    pslot->rsc[RSC_LEDS].name = FN_LEDS;
    pslot->rsc[RSC_LEDS].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_LEDS].bkey = 0;
    pslot->rsc[RSC_LEDS].pgscb = usercmd;
    pslot->rsc[RSC_LEDS].uilock = -1;
    pslot->rsc[RSC_LEDS].slot = pslot;
    pslot->name = "bb4io";
    pslot->desc = "The buttons and LEDs on the Baseboard";
    pslot->help = README;

    // Send current value to LEDs since on a daemon restart the
    // FPGA value of the LEDs may out of sync with our default.
    // Ignore return value since there's no user connection and
    // system errors are sent to the logger.
    (void) ledstofpga(pctx);

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,      // handle for our slot's internal info
    DP_PKT  *pkt,        // the received packet
    int      len)        // number of bytes in the received packet
{
    BB4IODEV *pctx;      // our local info
    RSC     *prsc;       // pointer to this slots button resource
    char     buttons[9]; // ASCII value of buttons "xx\n"
    int      buttonlen;  // #chars in buttons, should be 3

    pctx = (BB4IODEV *)(pslot->priv);  // Our "private" data is a BB4IODEV
    prsc = &(pslot->rsc[RSC_BUTTONS]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Do a sanity check on the received packet.
    if ((pkt->reg != BB4IO_REG_BUTTONS) || (pkt->count != 1)) {
        edlog("invalid bb4io packet from board to host");
        return;
    }

    // If a read response from a user dpget command, send value to UI
    if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) {
        buttonlen = sprintf(buttons, "%02x\n", pkt->data[0]);
        send_ui(buttons, buttonlen, prsc->uilock);
        prompt(prsc->uilock);
        // Response sent so clear the lock
        prsc->uilock = -1;
        del_timer(pctx->ptimer);  //Got the response
        pctx->ptimer = 0;
        return;
    }

    // Process of elimination makes this an autosend button update.
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey != 0) {
        buttonlen = sprintf(buttons, "%02x\n", pkt->data[0]);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(buttons, buttonlen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * usercmd():  - The user is reading or writing to the LEDs or
 * reading the buttons.
 * Get the value and update the LEDs on the BaseBoard or read the
 * value and write it into the supplied buffer.
 **************************************************************/
static void usercmd(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    BB4IODEV *pctx;    // our local info
    DP_PKT    pkt;     // send write and read cmds to the bb4io
    int       ret;     // return count
    int       newleds; // new value to assign the leds
    int       txret;   // ==0 if the packet went out OK
    CORE     *pmycore; // FPGA peripheral info


    pctx = (BB4IODEV *) pslot->priv;
    pmycore = pslot->pcore;


    if ((cmd == EDGET) && (rscid == RSC_LEDS)) {
        ret = snprintf(buf, *plen, "%02x\n", pctx->ledval);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDSET) && (rscid == RSC_LEDS)) {
        ret = sscanf(val, "%x", &newleds);
        if ((ret != 1) || (newleds < 0) || (newleds > 0xff)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->ledval = newleds;

        txret =  ledstofpga(pctx);   // This peripheral's context
        if (txret != 0) {
            // the send of the new LEDs value did not succeed.  This
            // probably means the input buffer to the USB port is full.
            // Tell the user of the problem.
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        // Start timer to look for a write response.
        if (pctx->ptimer == 0) {
            pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
        }
    }
    if ((cmd == EDGET) && (rscid == RSC_BUTTONS)) {
        // create a read packet to get the current value of the pins
        pkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = BB4IO_REG_BUTTONS;
        pkt.count = 1;

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
        pslot->rsc[RSC_BUTTONS].uilock = (char) cn;

        // Nothing to send back to the user
        *plen = 0;
    }
    return;
}


/**************************************************************
 * ledstofpga():  - Send LED value to the FPGA card.  Return
 * zero on success
 **************************************************************/
static int ledstofpga(
    BB4IODEV *pctx)    // This peripheral's context
{
    DP_PKT   pkt;      // send write and read cmds to the bb4io
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pslot;
    pmycore = (CORE *)pmyslot->pcore;

    // Got a new value for the LEDs.  Send down to the card.
    // Build and send the write command to set the LEDs
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = BB4IO_REG_LEDS;
    pkt.count = 1;
    pkt.data[0] = pctx->ledval;
    txret = dpi_tx_pkt(pmycore, &pkt, 5);
    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    BB4IODEV *pctx)    // Send LEDs of this bb4io to the FPGA
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}
// end of bb4io.c
