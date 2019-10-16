/*
 *  Name: qpot4.c
 *
 *  Description: Driver for the qpot4 peripheral
 *
 *  Hardware Registers: (same as the generic SPI peripheral)
 *    Addr=0    Clock select, chip select control, interrupt control and
 *              SPI mode register
 *    Addr=1    Max addr of packet data (== SPI pkt sz + 1)
 *    Addr=2    Data byte #1 in/out
 *    Addr=3    Data byte #2 in/out
 *    Addr=4    Data byte #3 in/out
 *        ::              ::
 *    Addr=14   Data byte #13 in/out
 *    Addr=15   Data byte #14 in/out
 *
 *  NOTES:
 *   - The RAM addresses are numbered from zero and the first two locations
 *     are mirrors of the two config registers.  Thus the actual SPI packet
 *     data starts at addr=2 and goes up to (SPI_pkt_sz + 1).  This means
 *     that at most 14 bytes can be sent at one time.  
 *   - Extend the number of bytes in a packet by forcing CS low and sending
 *     several packets.  The electronics will see just one packet.
 *
 *  Resources:
 *    value     - read/write resource to set pot values as a percentage of FS
 *
 *
 * Copyright:   Copyright (C) 2017-2019 Demand Peripherals, Inc.
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
        // register definitions
#define QCSPI_REG_MODE     0x00
#define QCSPI_REG_COUNT    0x01
#define QCSPI_REG_SPI      0x02
        // ESPI definitions
#define CS_MODE_AL          0   // Active low chip select
#define CS_MODE_AH          1   // Active high chip select
#define CS_MODE_FL          2   // Forced low chip select
#define CS_MODE_FH          3   // Forced high chip select
#define CLK_2M              0   // 2 MHz
#define CLK_1M              1   // 1 MHz
#define CLK_500K            2   // 500 KHz
#define CLK_100K            3   // 100 KHz
        // misc constants
#define MAX_LINE_LEN        100
#define FN_VALUE            "value"
        // Resource index numbers
#define RSC_VALUE           0

// qpot local context
typedef struct
{
    SLOT    *pSlot;         // handle to peripheral's slot info
    int      flowCtrl;      // ==1 if we are applying flow control
    int      xferpending;   // ==1 if we are waiting for a reply
    void    *ptimer;        // Watchdog timer to abort a failed transfer
    int      pot0;          // Value of pot in range of 0-257 (1-100%)
    int      pot1;          // Value of pot in range of 0-257 (1-100%)
    int      pot2;          // Value of pot in range of 0-257 (1-100%)
    int      pot3;          // Value of pot in range of 0-257 (1-100%)
} QPOTDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void  packet_hdlr(SLOT *, DP_PKT *, int);
static void  get_values(int, int, char*, SLOT*, int, int*, char*);
static int   send_spi(QPOTDEV*);
static void  noAck(void *, QPOTDEV*);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    QPOTDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (QPOTDEV *) malloc(sizeof(QPOTDEV));
    if (pctx == (QPOTDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in espi initialization");
        return (-1);
    }

    pctx->pSlot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_VALUE].name = FN_VALUE;
    pslot->rsc[RSC_VALUE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_VALUE].bkey = 0;
    pslot->rsc[RSC_VALUE].pgscb = get_values;
    pslot->rsc[RSC_VALUE].uilock = -1;
    pslot->rsc[RSC_VALUE].slot = pslot;
    pslot->name = "qpot";
    pslot->desc = "quad digital potentiometer";
    pslot->help = README;

    return (0);
}


/**************************************************************
 * Handle incoming packets from the peripheral.
 * Check for unexpected packets, discard write response packet,
 * send read response packet data to UI.
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,    // handle for our slot's internal info
    DP_PKT  *pkt,      // the received packet
    int      len)      // number of bytes in the received packet
{
    QPOTDEV *pctx;     // our local info

    pctx = (QPOTDEV *)(pslot->priv);  // Our "private" data is a QPOTDEV

    // Packets are either a write reply or an auto send SPI reply.
    // The auto-send packet should have a count two (for the 2 config bytes)
    // and the number of bytes in the SPI packet (nbxfer).
    // (count = 9 = count plus two per pot times four pots)
    if (!(( //autosend packet (you'll get a autosend on all data writes)
           ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 16))
          ||    ( // write response packet for mosi data packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_COUNT) && (pkt->count == 9)))) {
        edlog("invalid qpot4 packet from board to host");
        return;
    }

    // just a write reply, return.
    if (pctx->ptimer) {
        del_timer(pctx->ptimer);  // Got the ACK
        pctx->ptimer = 0;
    }
    return;
}


/**************************************************************
 * Callback used to handle value resource from UI.
 * Read dpset parameters and send them to the peripheral.
 * On dpget, return current configuration to UI.
 **************************************************************/
static void get_values(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    char     ibuf[MAX_LINE_LEN];
    float    v0,v1,v2,v3;
    int      outlen;
    int      txret;

    RSC *prsc = &(pslot->rsc[RSC_VALUE]);
    QPOTDEV *pctx = pslot->priv;

    if (cmd == EDSET) {
        if ((sscanf(val, "%f %f %f %f\n", &v0, &v1, &v2, &v3) != 4)
           || (v0 < 0.0) || (v0 > 100.0)
           || (v1 < 0.0) || (v1 > 100.0)
           || (v2 < 0.0) || (v2 > 100.0)
           || (v3 < 0.0) || (v3 > 100.0)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // Save the values in range of 0-257
        if (v0 == 100.0)
            pctx->pot0 = 257;
        else if (v0 == 0)
            pctx->pot0 = 0;
        else
            pctx->pot0 = (int)(v0 * 0.257);
        if (v1 == 100.0)
            pctx->pot1 = 257;
        else if (v1 == 0)
            pctx->pot1 = 0;
        else
            pctx->pot1 = (int)(v1 * 0.257);
        if (v2 == 100.0)
            pctx->pot2 = 257;
        else if (v2 == 0)
            pctx->pot2 = 0;
        else
            pctx->pot2 = (int)(v2 * 0.257);
        if (v3 == 100.0)
            pctx->pot3 = 257;
        else if (v3 == 0)
            pctx->pot3 = 0;
        else
            pctx->pot3 = (int)(v3 * 0.257);

        txret = send_spi(pctx);

        if (txret != 0) {
            *plen = snprintf(buf, *plen, E_WRFPGA);
            // (errors are handled in calling routine)
            return;
        }
    }
    else {
        // write out the current values
        outlen = snprintf(ibuf, MAX_LINE_LEN, "%4.1f %4.1f %4.1f %4.1f\n", 
                          (float)(pctx->pot0 * 100.0 / 257.0),
                          (float)(pctx->pot1 * 100.0 / 257.0),
                          (float)(pctx->pot2 * 100.0 / 257.0),
                          (float)(pctx->pot3 * 100.0 / 257.0));

        prsc->uilock = (char) cn;
        send_ui(ibuf, outlen, prsc->uilock);
        prompt(prsc->uilock);
        prsc->uilock = -1;

    }

    return;
}


/**************************************************************
 * Function to handle actual SPI data transfer to peripheral.
 * Returns 0 on success, or negative tx_pkt() error code.
 **************************************************************/
static int send_spi(
    QPOTDEV *pctx)    // This peripheral's context
{
    DP_PKT   pkt;
    SLOT    *pslot;    // pointer to slot info.
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK


    // create a write packet to set the mode reg
    pslot = pctx->pSlot;
    pmycore = pslot->pcore;
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;

    pkt.reg = QCSPI_REG_COUNT;
    pkt.count = 1 + (2 * 4);       // sending count plus all SPI pkt bytes
    pkt.data[0] = pkt.count;       // max RAM address in the peripheral

    // Load the pot values into the SPI packet.
    // 16 bits per pot: high four are the address (0-3 = pot#),
    // next two bits are 00 for a write, and the rest are the
    // pot value.
    pkt.data[1] = 0x00 + ((pctx->pot0 >> 8) & 0x01);
    pkt.data[2] = pctx->pot0 & 0xff;
    pkt.data[3] = 0x10 + ((pctx->pot1 >> 8) & 0x01);
    pkt.data[4] = pctx->pot1 & 0xff;
    pkt.data[5] = 0x60 + ((pctx->pot2 >> 8) & 0x01);
    pkt.data[6] = pctx->pot2 & 0xff;
    pkt.data[7] = 0x70 + ((pctx->pot3 >> 8) & 0x01);
    pkt.data[8] = pctx->pot3 & 0xff;

    // try to send the packet.  Schedule a resend on tx failure
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);

    return txret;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    QPOTDEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

//end of qpot.c
