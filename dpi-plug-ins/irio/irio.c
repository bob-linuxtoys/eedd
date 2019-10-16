/*
 *  Name: irio.c
 *
 *  Description: Driver for the consumer IR receiver/transmitter
 *
 *  Resources:
 *    recv         - broadcast (dpcat ONLY) received IR packets in hex
 *    xmit         - 32 bit hex value to send.  Works with dpset only
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
        // IRIO register definitions
        // IR register definitions
#define REG_DATA      0
#define REG_POLARITY 32
#define REG_STATUS   32
        // line length from user to send an IR packet "0x12345678\n"
#define IRSTRLEN            12
#define FN_XMIT             "xmit"
#define FN_RECV             "recv"
        // Resource index numbers
#define RSC_RECV            0
#define RSC_XMIT            1


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an irio
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
} IRIODEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userxmit(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, IRIODEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    IRIODEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (IRIODEV *) malloc(sizeof(IRIODEV));
    if (pctx == (IRIODEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in irio initialization");
        return (-1);
    }

    // Init our IRIODEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_RECV].name = "recv";
    pslot->rsc[RSC_RECV].flags = CAN_BROADCAST;
    pslot->rsc[RSC_RECV].bkey = 0;
    pslot->rsc[RSC_RECV].pgscb = 0;      // no get/set callback
    pslot->rsc[RSC_RECV].uilock = -1;
    pslot->rsc[RSC_RECV].slot = pslot;
    pslot->rsc[RSC_XMIT].name = "xmit";
    pslot->rsc[RSC_XMIT].flags = IS_WRITABLE;
    pslot->rsc[RSC_XMIT].bkey = 0;
    pslot->rsc[RSC_XMIT].pgscb = userxmit;
    pslot->rsc[RSC_XMIT].uilock = -1;
    pslot->rsc[RSC_XMIT].slot = pslot;
    pslot->name = "irio";
    pslot->desc = "Consumer IR receiver and transmitter ";
    pslot->help = README;

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,    // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    IRIODEV *pctx;     // our local info
    RSC    *prsc;      // pointer to this slots IR receiver resource
    int     recvval;   // received IR data
    char    recv[IRSTRLEN]; // ASCII value of IR recv data
    int     recvlen;   // #char in recv
    int     i;         // generic loop counter

    pctx = (IRIODEV *)(pslot->priv);  // Our "private" data is an IRIODEV
    prsc = &(pslot->rsc[RSC_RECV]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // Do a sanity check on the received packet.
    if ((pkt->reg != REG_DATA) || (pkt->count != 32)) {
        edlog("invalid irio packet from board to host");
        return;
    }

    // Just return if no one is monitoring the IR receiver
    if (prsc->bkey == 0) {
        return;
    }

    // Pull the bits out of the 32 bytes and sent result to listeners
    recvval = 0;
    for (i = 0; i < 32 ; i++) {
        recvval = recvval << 1;
        recvval += pkt->data[i];
    }
    recvlen = snprintf(recv, IRSTRLEN, "0x%08x\n", recvval);
    bcst_ui(recv, recvlen, &(prsc->bkey));

    return;
}


/**************************************************************
 * userxmit():  - The user is sending an IR packet.  Get the
 * value, break it into individual bits, and send it down to
 * the FPGA card.
 **************************************************************/
static void userxmit(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    DP_PKT   pkt;      // packet to the FPGA
    IRIODEV *pctx;     // our local info
    CORE    *pmycore;  // FPGA peripheral info
    int      ret;      // return count
    int      xmitval;  // 32 bits to send to the IR transmitter
    int      txret;    // ==0 if the packet went out OK
    int      i;        // generic loop counter 

    pctx = (IRIODEV *) pslot->priv;
    pmycore = pslot->pcore;

    ret = sscanf(val, "%x", &xmitval);
    if (ret != 1) {
        ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
        *plen = ret;
        return;
    }

    // Build and send the write command to send IR xmit data.
    // See the protocol manual for a description of the registers.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = REG_DATA;
    pkt.count = 32;

    /* Break the value into bits and load the 32 bits in to registers.
     * The 32 bits are the address, the address inverted, the command,
     * and the command inverted.  Address and command are send LSB first.
     * The value from the user has the command in the low byte and the
     * address in the high byte.  We ignore data in any higher bytes. */
    for (i = 31; i >= 0; i--) {
        pkt.data[i] = xmitval & 0x0001;
        xmitval = xmitval >> 1;
    }

    // Packet is ready, now send it
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data
    if (txret != 0) {
        // the send of the IR packet did not succeed.  This
        // probably means the input buffer to the USB port is full.
        // Tell the user of the problem.
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);

    *plen = 0;   // no response to user on xmit

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void    *timer,   // handle of the timer that expired
    IRIODEV *pctx)    // the context of this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}
// end of irio.c
