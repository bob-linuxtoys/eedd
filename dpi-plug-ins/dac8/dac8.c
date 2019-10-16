/*
 *  Name: dac8.c
 *
 *  Description: Driver for the dac8 peripheral
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
 *    value     - read/write resource to set dac values as a percentage of FS
 *
 *
 * Copyright:   Copyright (C) 2019 Demand Peripherals, Inc.
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
#define RSC_VALUE           0    /* 8 bit value of a dac */
        // Number of dacs
#define NDAC                8
        // init states
#define ST_CONFIG_1         -3   /* Send power down release */
#define ST_CONFIG_2         -2   /* IO or DA select */
#define ST_CONFIG_3         -1   /* IO status */

// dac8 local context
typedef struct
{
    SLOT    *pSlot;         // handle to peripheral's slot info
    int      flowCtrl;      // ==1 if we are applying flow control
    int      xferpending;   // ==1 if we are waiting for a reply
    void    *ptimer;        // Watchdog timer to abort a failed transfer
    int      state;         // Init states or sending dac values
    int      dac[NDAC];     // Value of dac in range of 0-ff (1-100%)
} DAC8DEV;


/**************************************************************
 *  - Globals
 **************************************************************/
    // table to map dac index to BH2226 register
int d2r[] = {8, 4, 12, 2, 10, 6, 14, 1 };


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void  packet_hdlr(SLOT *, DP_PKT *, int);
static void  get_values(int, int, char*, SLOT*, int, int*, char*);
static int   send_spi(DAC8DEV*);
static void  noAck(void *, DAC8DEV*);
extern int   dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    DAC8DEV *pctx;     // our local device context
    int      txret;    // error/return status of tx send

    // Allocate memory for this peripheral
    pctx = (DAC8DEV *) malloc(sizeof(DAC8DEV));
    if (pctx == (DAC8DEV *) 0) {
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
    pslot->name = "dac8";
    pslot->desc = "octal digital to analog converter";
    pslot->help = README;


    pctx->state = ST_CONFIG_1; // Init by sending power down release
    txret = send_spi(pctx);
    if (txret != 0) {          // Failed to send init SPI packet ?
        edlog(E_WRFPGA);
    }

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
    DAC8DEV *pctx;     // our local info
    int      txret = 0;

    pctx = (DAC8DEV *)(pslot->priv);  // Our "private" data is a DAC8DEV

    // Packets are either a write reply or an auto send SPI reply.
    // The auto-send packet should always have a count two, for
    // the 2 SPI config bytes or 

    // (count = 3 = count plus two dac control bytes
    if (!(( //autosend packet (you'll get a autosend on all data writes)
           ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 16))
          ||    ( // write response packet for mosi data packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_COUNT) && (pkt->count == 3)))) {
        edlog("invalid dac8 packet from board to host");
        return;
    }

    // Do state machine that sends the initial configuration packets
    if (pctx->state == ST_CONFIG_1) {
        pctx->state = ST_CONFIG_2;
        txret = send_spi(pctx);
        if (txret != 0) {
            edlog(E_WRFPGA);
            return;
        }
    }
    else if (pctx->state == ST_CONFIG_2) {
        pctx->state = ST_CONFIG_3;
        txret = send_spi(pctx);
        if (txret != 0) {
            edlog(E_WRFPGA);
            return;
        }
    }
    else if (pctx->state == ST_CONFIG_3) {
        pctx->state = 0;
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
    int      didx;     // index of dac to set/get
    int      dval;     // the dac value
    int      outlen;
    int      txret = 0;

    DAC8DEV *pctx = pslot->priv;

    if (cmd == EDSET) {
        if ((sscanf(val, "%d %x\n", &didx, &dval) != 2)
           || (didx < 1) || (didx > NDAC)
           || (dval < 0) || (dval > 0xff)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // Save the value and which dac to update
        didx--;                      // convert range 1--8 to 0--7
        pctx->dac[didx] = dval;
        pctx->state = didx;

        txret = send_spi(pctx);

        if (txret != 0) {
            *plen = snprintf(buf, *plen, E_WRFPGA);
            // (errors are handled in calling routine)
            return;
        }
    }
    else {
        // write out the requested value
        if ((sscanf(val, "%d\n", &didx) != 1)
           || (didx < 1) || (didx > NDAC)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        didx--;                      // convert range 1--8 to 0--7
        outlen = snprintf(ibuf, MAX_LINE_LEN, "%02x\n", pctx->dac[didx]);
        *plen = outlen;
    }

    return;
}


/**************************************************************
 * Function to handle actual SPI data transfer to peripheral.
 * Returns 0 on success, or negative tx_pkt() error code.
 **************************************************************/
static int send_spi(
    DAC8DEV *pctx)    // This peripheral's context
{
    DP_PKT   pkt;
    SLOT    *pslot;    // handle to peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK


    // create a write packet to set the mode reg
    pslot = pctx->pSlot;
    pmycore = pslot->pcore;
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = (pslot->pcore)->core_id;

    pkt.reg = QCSPI_REG_COUNT;
    pkt.count = 1 + (2);           // sending count plus two dac bytes
    pkt.data[0] = pkt.count;       // max RAM address in the peripheral

    // Load the config or dac values into the SPI packet.
    if (pctx->state == ST_CONFIG_1) {
        // Send power down release
        pkt.data[1] = 0x09;
        pkt.data[2] = 0xff;
    }
    else if (pctx->state == ST_CONFIG_2) {
        // IO or DA select
        pkt.data[1] = 0x03;
        pkt.data[2] = 0xff;
    }
    else if (pctx->state == ST_CONFIG_3) {
        // IO status
        pkt.data[1] = 0x0f;
        pkt.data[2] = 0xff;
    }
    else {
        // else state is the index of the dac to send to the card
        // The mapping of dac to register is a little strange for the
        // BH2226.  Use a table to translate dac index to register.
        pkt.data[1] = d2r[pctx->state];
        pkt.data[2] = pctx->dac[pctx->state];
    }

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
    DAC8DEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

//end of dac8.c
