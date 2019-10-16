/*
 *  Name: gpio4.c
 *
 *  Description: Driver for the Quad General Purpose Input/Output peripheral
 *
 *  Hardware Registers:
 *    0: pins      - 4-bit read/write/monitor
 *    1: dir       - 4-bit pin direction
 *    2: intr      - 4-bit interrupt mask
 * 
 *  Resources:
 *    pins         - read/write/broadcast ASCII data to/from pins
 *    direction    - pin direction, 0==input, 1==output
 *    interrupt    - interrupt (autosend) on change, 0==poll, 1==autosend
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

/*
 *    The Quad GPIO I/O Port provides four pins for either input or output.
 *  When configured as an input, each pin may additionally be configured to
 *  send up to the host the new value of the input when the input changes.
 *
 *  Registers:
 *    The Quad GPIO I/O Port uses three 8-bit registers for control
 *  and status, as follows.
 *
 *    0:  A read of Register 0 returns the current values at all four
 *        pins and a write sets the values for those pins configured
 *        as outputs.  Bit 0 in each register corresponds to the lowest
 *        numbered pin on the BaseBoard connector.
 *
 *    1:  Direction register with a one indicating an output and a zero
 *        indicating an input.  All four pins default to inputs after power up.
 *
 *    2:  Interrupt-on-change configuration register. Here an "interrupt"
 *        refers to asynchronously sending to the host a USB packet that
 *        contains the most recent values available at the pins.  Setting
 *        a bit to one enables interrupt-on-change for the corresponding
 *        pin.  The power up default is to turn off interrupt-on-change.
 *        A change on an enabled interrupt pin causes the peripheral to
 *        send to the host a read response packet for a read of one 8-bit
 *        register starting at register 0.
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
        // GPIO4 register definitions
#define GPIO4_REG_PINS     0x00
#define GPIO4_REG_DIR      0x01
#define GPIO4_REG_INTR     0x02
        // resource names and numbers
#define FN_PINS            "pins"
#define FN_DIR             "direction"
#define FN_INTR            "interrupt"
#define RSC_PINS           0
#define RSC_DIR            1
#define RSC_INTR           2


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an gpio4
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      pinval;   // value of the (output) pins
    int      dir;      // pin direction (in=0, out=1)
    int      intr;     // autosend on change (no=0, yes=1)
    void    *ptimer;   // timer to watch for dropped ACK packets
} GPIO4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userpins(int, int, char*, SLOT*, int, int*, char*);
static void userdir(int, int, char*, SLOT*, int, int*, char*);
static void userintr(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, GPIO4DEV *);
static void sendconfigtofpga(GPIO4DEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    GPIO4DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (GPIO4DEV *) malloc(sizeof(GPIO4DEV));
    if (pctx == (GPIO4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in gpio4 initialization");
        return (-1);
    }

    // Init our GPIO4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->pinval = 0xf;        // default value matches power up default
    pctx->dir = 0;             // all pins are inputs
    pctx->intr = 0;            // no autosend on change
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_PINS].name = FN_PINS;
    pslot->rsc[RSC_PINS].flags = IS_READABLE | IS_WRITABLE | CAN_BROADCAST;
    pslot->rsc[RSC_PINS].bkey = 0;
    pslot->rsc[RSC_PINS].pgscb = userpins;
    pslot->rsc[RSC_PINS].uilock = -1;
    pslot->rsc[RSC_PINS].slot = pslot;
    pslot->rsc[RSC_DIR].name = FN_DIR;
    pslot->rsc[RSC_DIR].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_DIR].bkey = 0;
    pslot->rsc[RSC_DIR].pgscb = userdir;
    pslot->rsc[RSC_DIR].uilock = -1;
    pslot->rsc[RSC_DIR].slot = pslot;
    pslot->rsc[RSC_INTR].name = FN_INTR;
    pslot->rsc[RSC_INTR].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_INTR].bkey = 0;
    pslot->rsc[RSC_INTR].pgscb = userintr;
    pslot->rsc[RSC_INTR].uilock = -1;
    pslot->rsc[RSC_INTR].slot = pslot;
    pslot->name = "gpio4";
    pslot->desc = "Quad General Purpose Input/Output";
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
    SLOT   *pslot,     // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    GPIO4DEV *pctx;    // our local info
    RSC    *prsc;      // pointer to this slot's pins resource
    char    pinstr[10];  // pin value as a single hex digit
    int     pinlen;    // length of pin value string (should be 2) 

    pctx = (GPIO4DEV *)(pslot->priv);  // Our "private" data is a GPIO4DEV
    prsc = &(pslot->rsc[RSC_PINS]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the pins should come in since we don't ever read the direction
    // or interrupt registers.
    if ((pkt->reg != GPIO4_REG_PINS) || (pkt->count != 1)) {
        edlog("invalid gpio4 packet from board to host");
        return;
    }

    // If a read response from a user dpget command, send value to UI
    if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) {
        pinlen = sprintf(pinstr, "%1x\n", (pkt->data[0] & 0x0f));
        send_ui(pinstr, pinlen, prsc->uilock);
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
        pinlen = sprintf(pinstr, "%1x\n", (pkt->data[0] & 0x0f));
        send_ui(pinstr, pinlen, prsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(pinstr, pinlen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * userpins():  - The user is reading or writing the gpio pins
 * Get the value and update the pins on the BaseBoard or read the
 * value and write it into the supplied buffer.
 **************************************************************/
static void userpins(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    GPIO4DEV *pctx;    // our local info
    DP_PKT   pkt;      // packet to the FPGA card
    CORE    *pmycore;  // FPGA peripheral info
    int      ret;      // return count
    int      newpins;  // new value to assign the pins
    int      txret;    // ==0 if the packet went out OK

    pctx = (GPIO4DEV *) pslot->priv;
    pmycore = pslot->pcore;

    if (cmd == EDGET) {
        // create a read packet to get the current value of the pins
        pkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
        pkt.core = pmycore->core_id;
        pkt.reg = GPIO4_REG_PINS;
        pkt.count = 1;
        // send the packet.  Report any errors
        txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + 0 data on read
        if (txret != 0) {
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        // Start timer to look for a read response.
        if (pctx->ptimer == 0)
            pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
        // lock this resource to the UI session cn
        pslot->rsc[RSC_PINS].uilock = (char) cn;
        // Nothing to send back to the user
        *plen = 0;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%x", &newpins);
        if ((ret != 1) || (newpins < 0) || (newpins > 0xff)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->pinval = newpins;
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }

    return;
}


/**************************************************************
 * userdir():  - The user is reading or setting the direction.
 **************************************************************/
static void userdir(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    GPIO4DEV *pctx;    // our local info
    int      ret;      // return count
    int      newdir;   // new value to assign the direction

    pctx = (GPIO4DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%1x\n", pctx->dir);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%x", &newdir);
        if ((ret != 1) || (newdir < 0) || (newdir > 0xf)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->dir = newdir;
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }

    return;
}


/**************************************************************
 * userintr():  - The user is reading or setting the interrupt
 * mask.
 **************************************************************/
static void userintr(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    GPIO4DEV *pctx;    // our local info
    int      ret;      // return count
    int      newintr;  // new value to assign the interrupt

    pctx = (GPIO4DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%1x\n", pctx->intr);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%x", &newintr);
        if ((ret != 1) || (newintr < 0) || (newintr > 0xf)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->intr = newintr;
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send pin values to the FPGA card.  Put error
 * messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    GPIO4DEV *pctx,    // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the gpio4
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
    pkt.reg = GPIO4_REG_PINS;   // the first reg of the three
    pkt.count = 3;
    pkt.data[0] = pctx->pinval;
    pkt.data[1] = pctx->dir;
    pkt.data[2] = pctx->intr;
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
    GPIO4DEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of gpio4.c
