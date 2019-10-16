/*
 *  Name: espi.c
 *
 *  Description: Driver for the SPI peripheral
 *
 *  Hardware Registers:
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
 *    data      - read/write resource to send/receive SPI data
 *    config    - SPI port configuration including clock speed, and CS config
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
#define QCSPI_NDATA_BYTE   16   // num data registers from QCSPI_REG_SPI
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
#define FN_CFG              "config"
#define FN_DATA             "data"
        // Resource index numbers
#define RSC_DATA            0
#define RSC_CFG             1

// per-quadrature clocked SPI port info
typedef struct
{
    int      csmode;        // active high/low or forced high/low
    int      clksrc;        // The SCK frequency
    struct QCSPIDEV *pQcspi;  // peripheral for this spiport instance
} SPIPORT;

// espi local context
typedef struct
{
    SLOT    *pSlot;         // handle to peripheral's slot info
    int      flowCtrl;      // ==1 if we are applying flow control
    int      xferpending;   // ==1 if we are waiting for a reply
    void    *ptimer;        // Watchdog timer to abort a failed transfer
    int      nbxfer;        // Number of bytes in most recent SPI packet sent
    unsigned char bxfer[QCSPI_NDATA_BYTE]; // the bytes to send
    SPIPORT  spiport;       // spi port dev info
} QCSPIDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void  packet_hdlr(SLOT *, DP_PKT *, int);
static void  cb_data(int, int, char*, SLOT*, int, int*, char*);
static void  cb_config(int, int, char*, SLOT*, int, int*, char*);
static int   send_spi(QCSPIDEV*);
static void  no_ack(void *, QCSPIDEV*);
extern int   dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    QCSPIDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (QCSPIDEV *) malloc(sizeof(QCSPIDEV));
    if (pctx == (QCSPIDEV *) 0) {
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
    pslot->rsc[RSC_DATA].name = FN_DATA;
    pslot->rsc[RSC_DATA].flags = IS_READABLE;
    pslot->rsc[RSC_DATA].bkey = 0;
    pslot->rsc[RSC_DATA].pgscb = cb_data;
    pslot->rsc[RSC_DATA].uilock = -1;
    pslot->rsc[RSC_DATA].slot = pslot;
    pslot->rsc[RSC_CFG].name = FN_CFG;
    pslot->rsc[RSC_CFG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CFG].bkey = 0;
    pslot->rsc[RSC_CFG].pgscb = cb_config;
    pslot->rsc[RSC_CFG].uilock = -1;
    pslot->rsc[RSC_CFG].slot = pslot;
    pslot->name = "espi";
    pslot->desc = "a generic SPI interface";
    pslot->help = README;

    return (0);
}


/**************************************************************
 * Handle incoming packets from the peripheral.
 * Check for unexpected packets, discard write response packet,
 * send read response packet data to UI.
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{

    RSC    *prsc;
    QCSPIDEV *pCtx;

    prsc = &(pslot->rsc[RSC_DATA]);
    pCtx = (QCSPIDEV *)(pslot->priv);

    // Packets are either a write reply or an auto send SPI reply.
    // The auto-send packet should have a count two (for the 2 config bytes)
    // and the number of bytes in the SPI packet (nbxfer).
    if (!(( //autosend packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 16))
          ||    ( // write response packet for mosi data packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_COUNT) && (pkt->count == (1 + pCtx->nbxfer)))
          ||     ( // write response packet for config
           (((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 1))) ) ) {
        // unknown packet
        edlog("invalid espi packet from board to host");
        return;
    }

    // Return if just the write reply
    if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA)
        return;

    // Send data to UI
    char ob[MAX_LINE_LEN*3] = {0};
    int ob_len = 0;
    int i = 2; // skip the 2 config bytes
    for(;i< 2+pCtx->nbxfer;i++) {
        sprintf(&ob[(i - 2) * 3],"%02x ", pkt->data[i]);
    }
    sprintf(&ob[(i - 2) * 3], "\n");
    ob_len = ((i - 2) * 3) + 1;
    send_ui(ob, ob_len, prsc->uilock);
    prompt(prsc->uilock);

    // Response sent so clear the lock
    prsc->uilock = -1;
    del_timer(pCtx->ptimer);  //Got the response
    pCtx->ptimer = 0;
    return;
}


/**************************************************************
 * Callback used to handle data resource from UI.
 * Read dpget parameters and send them to the peripheral.
 * Response packets will be handled in packet_hdlr().
 **************************************************************/
static void cb_data(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    char      *pbyte;
    int        tmp;

    int txret;

    if(cmd == EDGET) {
        QCSPIDEV *pCtx = pslot->priv;
        // Get the bytes to send
        pCtx->nbxfer = 0;
        pCtx->pSlot = pslot;
        pbyte = strtok(val, ", ");
        while (pbyte) {
            sscanf(pbyte, "%x", &tmp);
            pCtx->bxfer[pCtx->nbxfer] = (unsigned char) (tmp & 0x00ff);
            pbyte = strtok((char *) 0, ", ");   // commas or spaces accepted
            pCtx->nbxfer++;
            if (pCtx->nbxfer == (QCSPI_NDATA_BYTE - 2))
                break;
        }

        if (pCtx->nbxfer != 0) {
            txret = send_spi(pCtx);
            if (txret != 0) {
                *plen = snprintf(buf, *plen, E_WRFPGA);
                // (errors are handled in calling routine)
                return;
            }

            // Start timer to look for a read response.
            if (pCtx->ptimer == 0)
                pCtx->ptimer = add_timer(ED_ONESHOT, 100, no_ack, (void *) pCtx);

            // lock this resource to the UI session cn
            pslot->rsc[RSC_DATA].uilock = (char) cn;

            // Nothing to send back to the user
            *plen = 0;
        }
        else {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
    }

    return;
}


/**************************************************************
 * Callback used to handle config resource from UI.
 * Read dpset parameters and send them to the peripheral.
 * On dpget, return current configuration to UI.
 **************************************************************/
static void cb_config(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    SPIPORT *pSPIport;
    int      newclk = -1;
    int      newcsmode = -1;
    char     ibuf[MAX_LINE_LEN];
    int      outlen = 0;

    RSC *prsc = &(pslot->rsc[RSC_DATA]);
    QCSPIDEV *pCtx = pslot->priv;
    pSPIport = (SPIPORT *) &(pCtx->spiport);

    if (cmd == EDSET) {
        if (sscanf(val, "%d %s\n", &newclk, ibuf) != 2) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        if (!strncmp(ibuf, "al", 2))
            newcsmode = CS_MODE_AL;
        else if (!strncmp(ibuf, "ah", 2))
            newcsmode = CS_MODE_AH;
        else if (!strncmp(ibuf, "fl", 2))
            newcsmode = CS_MODE_FL;
        else if (!strncmp(ibuf, "fh", 2))
            newcsmode = CS_MODE_FH;
        // Sanity check on the inputs
        if ((newclk < 5000) ||
            (newcsmode < 0) || (newcsmode > 3)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }

        if (newclk >= 2000000)
            newclk = CLK_2M;
        else if (newclk >= 1000000)
            newclk = CLK_1M;
        else if (newclk >= 500000)
            newclk = CLK_500K;
        else
            newclk = CLK_100K;

        // Save new user defined configuration
        pSPIport->csmode  = newcsmode;
        pSPIport->clksrc  = newclk;

        // Send just the config so set nbxfer to zero and then send the packet
        pCtx->nbxfer = 0;

        int txret = send_spi(pCtx);

        if (txret != 0) {
            *plen = snprintf(buf, *plen, E_WRFPGA);
            // (errors are handled in calling routine)
            return;
        }
    }
    else {
        // write out the current configuration
        if (pSPIport->clksrc == CLK_2M)
            newclk = 2000000;
        else if (pSPIport->clksrc == CLK_1M)
            newclk = 1000000;
        else if (pSPIport->clksrc == CLK_500K)
            newclk = 500000;
        else
            newclk = 100000;
        if (pSPIport->csmode == CS_MODE_AL)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d al\n", newclk);
        else if (pSPIport->csmode == CS_MODE_AH)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d ah\n", newclk);
        else if (pSPIport->csmode == CS_MODE_FL)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d fl\n", newclk);
        else if (pSPIport->csmode == CS_MODE_FH)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d fh\n", newclk);

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
    QCSPIDEV *pCtx)    // This peripheral's context
{
    DP_PKT   pkt;
    SLOT    *pmyslot;  // Our per slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      i;
    SPIPORT *pSPIport;

    pSPIport = &(pCtx->spiport);
    pmyslot = pCtx->pSlot;
    pmycore = pmyslot->pcore;

    // create a write packet to set the mode reg
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;

    if (pCtx->nbxfer == 0) {
        // send the clock source and SPI mode
        pkt.data[0] = (pSPIport->clksrc << 6) | (pSPIport->csmode << 2) ;
        pkt.reg = QCSPI_REG_MODE;
        pkt.count = 1;
    }
    else {
        pkt.reg = QCSPI_REG_COUNT;
        pkt.count = 1 + pCtx->nbxfer;  // sending count plus all SPI pkt bytes
        pkt.data[0] = 1 + pCtx->nbxfer;  // max RAM address in the peripheral

        // Copy the SPI packet to the DP packet data
        for (i = 0; i < pCtx->nbxfer; i++)
            pkt.data[i + 1] = pCtx->bxfer[i];
    }

    // try to send the packet.  Schedule a resend on tx failure
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    return txret;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void no_ack(
    void     *timer,   // handle of the timer that expired
    QCSPIDEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

//end of espi.c
