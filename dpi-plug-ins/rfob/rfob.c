/*
 *  Name: rfob.c
 *
 *  Description: Driver for RF keyfob transmitter decoder
 *
 *  Hardware Registers:
 *    0: cmds      - 4-bit read/write/monitor
 *    1: dir       - 4-bit pin direction
 *    2: config      - 4-bit interrupt mask
 *   00: Data bit 0
 *      ::: ::
 *   31: Data bit 31
 *   32: Number of valid bits in packet
 *   33: Number of 10us samples in a bit time.  Determines BPS.
 *
 *      The keyfob receiver card has a 315 MHz receiver and a circuit
 *  to convert the levels at the receiver to 3.3 volts.
 *  Pin 1 is the Rx data line from the receiver.
 *  Pin 3 is RSSI from the receiver (but is unused)
 *  Pin 5 is an LED that indicates the start of a packet
 *  Pin 7 is an LED that toggles on completion of a valid packet
 *
 *  
 *  HOW THIS WORKS
 *      Keyfob transmitters encode the bits in a frame using PWM.  The
 *  actual pulse widths depend on the data rate.  Higher data rates have
 *  shorter pulses than those of lower data rates.  A 1700 bps transmitter
 *  has a bit width of 600 us with a zero bit width of 150 us and one
 *  width of 450 us.  A 560 bsp transmitter has bit widths for zero and one
 *  as 400 us and 1.2 ms, with a bit width of 1.8 ms.  Register 33 tells
 *  us how many samples to sum before deciding if the sample represent a
 *  one or a zero.
 *      A "command" is a complete sequence of data pulses with a leading
 *  sync interval.  The sync interval is always at least 10 milliseconds
 *  long.  The first edge after the sync interval is the start of bit #0.
 *  There can be a variable number of bits in a packet depending on the
 *  type of keyfob transmitter used.  The end of a packet is defined as
 *  the first interval without an edge, positive or negative, for one
 *  bit time.  We check for a valid packet by comparing the number of
 *  bits received to the number we expect.  We toggle the green LED if
 *  the command packet is valid.
 * 
 *  Resources:
 *    cmds         - received commands as a hex number
 *    config       - number of bits in command and the baudrate
 *
 * Copyright:   Copyright (C) 2018-2019 Demand Peripherals, Inc.
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
        // RFOB register definitions
#define RFOB_REG_CMDS     0x00
#define RFOB_REG_BITS     0x20
#define RFOB_REG_SMPLS    0x21

        // resource names and numbers
#define FN_CMDS            "cmds"
#define FN_CONFIG          "config"
#define RSC_CMDS           0
#define RSC_CONFIG         1

        // Sane default values
#define DEF_BITS           24
#define DEF_BAUD           560


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an rfob
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      bits;     // number of bits per command packet
    int      baud;     // number of bits per second in data stream
    void    *ptimer;   // timer to watch for dropped ACK packets
} RFOBDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userconfig(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, RFOBDEV *);
static void sendconfigtofpga(RFOBDEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    RFOBDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (RFOBDEV *) malloc(sizeof(RFOBDEV));
    if (pctx == (RFOBDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in rfob initialization");
        return (-1);
    }

    // Init our RFOBDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->bits = DEF_BITS;     // bits per command
    pctx->baud = DEF_BAUD;     // bits per second
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_CMDS].name = FN_CMDS;
    pslot->rsc[RSC_CMDS].flags = CAN_BROADCAST;
    pslot->rsc[RSC_CMDS].bkey = 0;
    pslot->rsc[RSC_CMDS].pgscb = 0;
    pslot->rsc[RSC_CMDS].uilock = -1;
    pslot->rsc[RSC_CMDS].slot = pslot;
    pslot->rsc[RSC_CONFIG].name = FN_CONFIG;
    pslot->rsc[RSC_CONFIG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CONFIG].bkey = 0;
    pslot->rsc[RSC_CONFIG].pgscb = userconfig;
    pslot->rsc[RSC_CONFIG].uilock = -1;
    pslot->rsc[RSC_CONFIG].slot = pslot;
    pslot->name = "rfob";
    pslot->desc = "Receiver/decoder for keychain remote controls";
    pslot->help = README;

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
    RFOBDEV *pctx;     // our local info
    RSC    *prsc;      // pointer to this slot's cmds resource
    int     i;         // counts from 0 to bits-1
    int     cmd = 0;   // the received command
    int     cmdlen;    // length of cmd value string 
    char    cmdstr[16]; // should be less than 'hhhhhhhh\n\0', or 10 chars


    pctx = (RFOBDEV *)(pslot->priv);  // Our "private" data is a RFOBDEV
    prsc = &(pslot->rsc[RSC_CMDS]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the cmds should come in since we don't ever read the bits
    // or baud registers.
    if ((pkt->reg != RFOB_REG_CMDS) || (pkt->count != pctx->bits)) {
        edlog("invalid rfob packet from board to host");
        return;
    }

    // Process of elimination makes this an autosend packet.
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey != 0) {
        // The bits are MSB to LSB.  Add them one at a time to cmd
        for (i = 0; i < pkt->count; i++) {
            cmd = cmd << 1;
            if (pkt->data[i] == 1)
                cmd += 1;
        }
        cmdlen = sprintf(cmdstr, "%x\n", cmd);
        send_ui(cmdstr, cmdlen, prsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(cmdstr, cmdlen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * userconfig():  - The user is reading or setting the configuration
 **************************************************************/
static void userconfig(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    RFOBDEV *pctx;     // our local info
    int      ret;      // return count
    int      newbits;  // new value to assign cmd length
    int      newbaud;  // new bps

    pctx = (RFOBDEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%d %d\n", pctx->bits, pctx->baud);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%d %d", &newbits, &newbaud);
        if ((ret != 2) ||
            (newbits < 1) || (newbits > 32) ||
            (newbaud < 300) || (newbaud > 4800)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->bits = newbits;
        pctx->baud = newbaud;
        sendconfigtofpga(pctx, plen, buf);  // send cmds, dir, config
    }

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send number of bits and baud rate to
 * card. 
 **************************************************************/
static void sendconfigtofpga(
    RFOBDEV *pctx,    // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the rfob
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Write the values for the cmds, direction, and interrupt mask
    // down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = RFOB_REG_BITS;   // the first reg of the two
    pkt.count = 2;
    pkt.data[0] = pctx->bits;
    // Register 33 is the number of samples at 10us to sum to decide
    // if the bit is zero or one.  We want to sample about 90 percent
    // of the bit time so we have time to wait for the start of the
    // next bit.  There are 100,000 10us samples in a second and we
    // want 90 percent of that.
    pkt.data[1] = 90000 / pctx->baud;
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
    RFOBDEV  *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of rfob.c
