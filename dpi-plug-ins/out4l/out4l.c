/*
 *  Name: out4l.c
 *
 *  Description: Device driver for the out4l peripheral
 *
 *  Hardware Registers:
 *    0: outval    - 8-bit read/write
 * 
 *  Resources:
 *    outval       - read/write ASCII data to four FPGA output pins
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
        // OUT4 register definitions
#define OUT4_REG_OUTVAL     0x00
        // line length from user to set OUT4 value ( e.g. "0x5\n")
#define OUTVAL_LEN          10
#define FN_OUTVAL           "outval"
        // Resource index numbers
#define RSC_OUTVAL          0


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an out4l
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    unsigned char outval; // Current value of the outputs
    void    *ptimer;   // timer to watch for dropped ACK packets
} OUT4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void out4luser(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, OUT4DEV *);
static int  out4ltofpga(OUT4DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    OUT4DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (OUT4DEV *) malloc(sizeof(OUT4DEV));
    if (pctx == (OUT4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in out4l initialization");
        return (-1);
    }

    // Init our OUT4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->outval = 0x0;        // Matches Verilog default value (==0 for out4l)
    pctx->ptimer = 0;          // set while waiting for a response

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_OUTVAL].name = "outval";
    pslot->rsc[RSC_OUTVAL].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_OUTVAL].bkey = 0;
    pslot->rsc[RSC_OUTVAL].pgscb = out4luser;
    pslot->rsc[RSC_OUTVAL].uilock = -1;
    pslot->rsc[RSC_OUTVAL].slot = pslot;
    pslot->name = "out4l";
    pslot->desc = "Four binary output pins";
    pslot->help = README;

    (void) out4ltofpga(pctx);

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
    OUT4DEV *pctx;    // our local info

    pctx = (OUT4DEV *)(pslot->priv);  // Our "private" data is a OUT4DEV

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Do a sanity check on the received packet.
    if ((pkt->reg != OUT4_REG_OUTVAL) || (pkt->count != 1)) {
        edlog("invalid out4l packet from board to host");
        return;
    }

    return;
}


/**************************************************************
 * out4luser():  - The user is reading or writing to the output.
 * Get the value and update the out4l on the BaseBoard or read the
 * value and write it into the supplied buffer.
 **************************************************************/
static void out4luser(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    OUT4DEV *pctx;    // our local info
    int      ret;      // return count
    int      newout4l;  // new value to assign the out4l
    int      txret;    // ==0 if the packet went out OK

    pctx = (OUT4DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%01x\n", pctx->outval);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    ret = sscanf(val, "%x", &newout4l);
    if ((ret != 1) || (newout4l < 0) || (newout4l > 0xf)) {
        ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
        *plen = ret;
        return;
    }
    pctx->outval = newout4l;

    txret =  out4ltofpga(pctx);   // This peripheral's context
    if (txret != 0) {
        // the send of the new outval did not succeed.  This probably
        // means the input buffer to the USB port is full.  Tell the
        // user of the problem.
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
 * out4ltofpga():  - Send outval to the FPGA card.  Return
 * zero on success
 **************************************************************/
int out4ltofpga(
    OUT4DEV *pctx)    // This peripheral's context
{
    DP_PKT   pkt;      // send write and read cmds to the out4l
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // Got a new value for the outputs.  Send down to the card.
    // Build and send the write command to set the out4l.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = OUT4_REG_OUTVAL;
    pkt.count = 1;
    pkt.data[0] = pctx->outval;
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void    *timer,   // handle of the timer that expired
    OUT4DEV *pctx)    // points to instance of this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}
// end of out4l.c
