/*
 *  Name: slide4
 *
 *  Description: Driver for the quad slide potentiometer card.  Output is a
 *        space separate list of positions that range from 0 to 1023.  The
 *        define SLIDE4_PERIOD sets the polling interval.  The FPGA sees
 *        this card as an instance of the adc812 card (but with 10 bits).
 *
 *  Hardware Registers:
 *        Addr=0/1  Channel 0 ADC value (high byte in reg 0, low in reg 1)
 *        Addr=2    Channel 1 ADC value
 *        Addr=4    Channel 2 ADC value
 *        Addr=6    Channel 3 ADC value
 *        Addr=8    Channel 4 ADC value
 *        Addr=10   Channel 5 ADC value
 *        Addr=12   Channel 6 ADC value
 *        Addr=14   Channel 7 ADC value
 *        Addr=16   Sample interval
 *        Addr=17   Differential input configuration
 *
 *  Resources:
 *        current   - the most recent set of values (dpget)
 *        samples   - a stream of values (dpcat)
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
        // a reading every 100 ms
#define SLIDE4_PERIOD     100
#define NPOT                4
        // SLIDE4 register definitions  (it's really an ADC812)
#define SLIDE4_REG_ADC0   0x00
#define SLIDE4_REG_CNFG   0x10

        // resource names and numbers
#define RSC_POSITIONS        0
        // Output string len = 4 * ("1234 ") - trailing space + newline
#define VALLEN             100
#define VALFMT             "%04d %04d %04d %04d\n"


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an adc812
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      value[NPOT];   // Most recent value
} SLIDE4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user(int, int, char*, SLOT*, int, int*, char*);
static int  tofpga(SLIDE4DEV *);
static void noAck(void *, SLIDE4DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    SLIDE4DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (SLIDE4DEV *) malloc(sizeof(SLIDE4DEV));
    if (pctx == (SLIDE4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in Slide4 initialization");
        return (-1);
    }

    // Init our SLIDE4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->value[0] = 0;
    pctx->value[1] = 0;
    pctx->value[2] = 0;
    pctx->value[3] = 0;


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_POSITIONS].name = "positions";
    pslot->rsc[RSC_POSITIONS].flags = IS_READABLE | CAN_BROADCAST;
    pslot->rsc[RSC_POSITIONS].bkey = 0;
    pslot->rsc[RSC_POSITIONS].pgscb = user;     // no get/set callback
    pslot->rsc[RSC_POSITIONS].uilock = -1;
    pslot->rsc[RSC_POSITIONS].slot = pslot;
    pslot->name = "slide4";
    pslot->desc = "Quad slide potentiometer";
    pslot->help = README;

    // Send the sample rate and sigle/differential configuration to FPGA.
    // Ignore return value since there's no user connection and
    // system errors are sent to the logger.
    (void) tofpga(pctx);  // send config

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
    SLIDE4DEV *pctx;   // our local info
    RSC    *prsc;      // pointer to this slot's samples resource
    char    valstr[VALLEN];  // adc values as space separated string
    int     slen;      // length of value string (should be 47) 
    int     i;
    int     sample, sample1, sample2;   // new value of pot reading
    int     changed = 0;  // set to 1 if the pot has moved


    pctx = (SLIDE4DEV *)(pslot->priv);  // Our "private" data is a SLIDE4DEV
    prsc = &(pslot->rsc[RSC_POSITIONS]);

    // Do a sanity check on the received packet.  Only reads from
    // the samples should come in since we don't ever read the period
    // or single/differential registers.
    if ((pkt->reg == SLIDE4_REG_ADC0) && (pkt->count == 16)) {
        // Get and save the new readings.  Record if any changed.
        for (i = 0; i < NPOT; i++) {
            sample1 = (pkt->data[(i*4)+1] + (pkt->data[(i*4)] << 8)) >> 3;
            sample2 = (pkt->data[(i*4)+3] + (pkt->data[(i*4)+2] << 8)) >> 3;
            sample  = (sample1 + sample2) / 2 ;
            changed = (sample == pctx->value[i]) ? changed : 1;
            pctx->value[i] = sample;
        }
        // Broadcast the readings if any UI are monitoring it.
        if ((changed) && (prsc->bkey != 0)) {
            slen = sprintf(valstr, VALFMT, pctx->value[0], pctx->value[1],
                 pctx->value[2], pctx->value[3]);
            send_ui(valstr, slen, prsc->uilock);
            // bkey will return cleared if UIs are no longer monitoring us
            bcst_ui(valstr, slen, &(prsc->bkey));
            return;
        }
    }
    // if not a data packet, is it the config ACK?
    else if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }
    // error if not a data packet or an ACK
    else {
        edlog("invalid slide4 packet from board to host");
        return;
    }
}


/**************************************************************
 * user():  - The user is reading the most recent values
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
    SLIDE4DEV *pctx;   // our local info
    int      ret;      // return count

    pctx = (SLIDE4DEV *) pslot->priv;

    if ((cmd == EDGET) && (rscid == RSC_POSITIONS)) {
        ret = snprintf(buf, *plen, VALFMT, pctx->value[0], pctx->value[1],
             pctx->value[2], pctx->value[3]);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    return;
}


/**************************************************************
 * tofpga():  - Send config values to the FPGA card.
 **************************************************************/
static int tofpga(
    SLIDE4DEV *pctx)   // This peripheral's context
{
    DP_PKT   pkt;      // send write and read cmds to the adc812
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // create a write packet to set the mode reg
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = SLIDE4_REG_CNFG;
    pkt.count = 2;
    pkt.data[0] = SLIDE4_PERIOD - 1;   // period is zero-indexed in the hardware 
    pkt.data[1] = 0;                   // no differential inputs

    // try to send the packet.  Apply or release flow control.
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
    void      *timer,   // handle of the timer that expired
    SLIDE4DEV *pctx)    // this peripheral's context
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of adc812.c
