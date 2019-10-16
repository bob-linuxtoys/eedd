/*
 * Name: rcrx.c
 *
 * Description: Decoder for the 6/8 channel RC receiver
 * 
 *  Hardware Registers:
 *      Reg 0:  Pulse #1 interval   (16 bits)  high or low, depending on sync polarity
 *      Reg 2:  Pulse #1 interval   (16 bits)  low or high
 *      Reg 4:  Pulse #2 interval   (16 bits)
 *      Reg 6:  Pulse #2 interval   (16 bits)
 *      Reg 8:  Pulse #3 interval   (16 bits)
 *      Reg 10: Pulse #3 interval   (16 bits)
 *      Reg 12: Pulse #4 interval   (16 bits)
 *      Reg 14: Pulse #4 interval   (16 bits)
 *      Reg 16: Pulse #5 interval   (16 bits)
 *      Reg 18: Pulse #5 interval   (16 bits)
 *      Reg 20: Pulse #6 interval   (16 bits)
 *      Reg 22: Pulse #6 interval   (16 bits)
 *      Reg 24: Pulse #7 interval   (16 bits)
 *      Reg 26: Pulse #7 interval   (16 bits)
 *      Reg 28: Pulse #8 interval   (16 bits)
 *      Reg 30: Pulse #8 interval   (16 bits)
 *      Reg 32: gpioval[7,6], gpiodir[5,4], unused[3], nchan[2-0]
 *
 *      The pulse interval registers have two fields.  The MSB is the
 *  value of the input during the interval being reported by the lower
 *  15 bits.  The lower 15 bits are the duration of the interval in units
 *  of 100 nanoseconds.  Even numbered registers are the low byte of the
 *  register
 *
 *      The low three bits of the configuration register specify the
 *  number of channels to expect in the received signal.  Bits 4 and 5
 *  set the GPIO direction and bit 6 and 7 set the (output) GPIO values.
 *  A one in the GPIO direction field makes the pin an output.
 *
 *      The first pin is the input from the RC receiver to the FPGA.  The
 *  second pin is an output that is high when an RC packed is being received.
 *  The second pin would usually be connected to an LED to show activity.
 *  The remaining two pins are used for general purpose I/O.  The low two
 *
 *  HOW THE FPGA WORKS
 *      Radio control systems encode the channel data as the position or
 *  width of a string of pulses.  A "frame" is a complete sequence of these
 *  pulses with a leading sync interval.  The sync interval is always at
 *  least 3 milliseconds long.  The first edge after the sync interval is
 *  the start of the pulse for channel #1.  The user value for a channel 
 *  is the time from the leading edge of the channel pulse to the leading
 *  edge of the next channel.  The signal may be inverted so we look for
 *  edges and not specific values.  This circuit records both the high
 *  and the low times for each pulse.  This additional information can be
 *  used by the host to help determine if the signal is valid or not.
 *      We send the data up to the host at an edge count of two times the
 *  number of channels if none of the intervals exceeded 3.2 milliseconds.
 *
 * 
 *  Resources:
 *    rcdata      - Decoded RC packets
 *    nchan       - The number of RC channels to expect
 *    gpiodir     - direction of GPIO pins
 *    gpioval     - read or write to get or set GPIO pins
 */

/*
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
#include <stdint.h>
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
        // RCRX register definitions
#define REG_RCDATA    0
#define REG_CONFIG   32
#define MASK_GPIOVAL  3
#define MAX_CHAN      8

        // resource names and numbers
#define FN_RCDATA          "rcdata"
#define FN_NCHAN           "nchan"
#define FN_GPIODIR         "gpiodir"
#define FN_GPIOVAL         "gpioval"
#define RSC_RCDATA         0
#define RSC_NCHAN          1
#define RSC_GPIODIR        2
#define RSC_GPIOVAL        3


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an rcrx  
typedef struct
{
    void    *pslot;       // handle to peripheral's slot info
    void    *ptimer;      // timer to watch for dropped ACK packets
    uint8_t  nchan;       // number of channels to expect
    uint8_t  gpiodir;     // direction of the two GPIO pins
    uint8_t  gpioval;     // value of the two GPIO pins
} RCRXDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userparm(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, RCRXDEV *);
static void sendconfigtofpga(RCRXDEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)          // points to the SLOT for this peripheral
{
    RCRXDEV *pctx;        // our local device context

    // Allocate memory for this peripheral
    pctx = (RCRXDEV *) malloc(sizeof(RCRXDEV));
    if (pctx == (RCRXDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in rcrx initialization");
        return (-1);
    }

    // Init our RCRXDEV structure
    pctx->pslot = pslot;  // our instance of a peripheral
    pctx->nchan = 8;      // default matches FPGA default value
    pctx->gpiodir = 0;    // default value matches power up default
    pctx->ptimer = 0;     // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_RCDATA].name = FN_RCDATA;
    pslot->rsc[RSC_RCDATA].flags = CAN_BROADCAST;
    pslot->rsc[RSC_RCDATA].bkey = 0;
    pslot->rsc[RSC_RCDATA].pgscb = 0;
    pslot->rsc[RSC_RCDATA].uilock = -1;
    pslot->rsc[RSC_RCDATA].slot = pslot;
    pslot->rsc[RSC_NCHAN].name = FN_NCHAN;
    pslot->rsc[RSC_NCHAN].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_NCHAN].bkey = 0;
    pslot->rsc[RSC_NCHAN].pgscb = userparm;
    pslot->rsc[RSC_NCHAN].uilock = -1;
    pslot->rsc[RSC_NCHAN].slot = pslot;
    pslot->rsc[RSC_GPIODIR].name = FN_GPIODIR;
    pslot->rsc[RSC_GPIODIR].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_GPIODIR].bkey = 0;
    pslot->rsc[RSC_GPIODIR].pgscb = userparm;
    pslot->rsc[RSC_GPIODIR].uilock = -1;
    pslot->rsc[RSC_GPIODIR].slot = pslot;
    pslot->rsc[RSC_GPIOVAL].name = FN_GPIOVAL;
    pslot->rsc[RSC_GPIOVAL].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_GPIOVAL].bkey = 0;
    pslot->rsc[RSC_GPIOVAL].pgscb = userparm;
    pslot->rsc[RSC_GPIOVAL].uilock = -1;
    pslot->rsc[RSC_GPIOVAL].slot = pslot;
    pslot->name = "rcrx";
    pslot->desc = "Eight Channel Radio Control Decoder";
    pslot->help = README;

    sendconfigtofpga(pctx, (int *) 0, (char *) 0);  // send config

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,     // handle for our slot's internal info
    DP_PKT  *pkt,       // the received packet
    int      len)       // number of bytes in the received packet
{
    RCRXDEV *pctx;           // our local info
    RSC       *prscval;      // pointer to gpioval resource
    RSC       *prscdat;      // pointer to rcdata resource
    char       cstr[200];    // space for four sets of int and float
    int        clen;         // length of times output string
    int        ret;          // return value for printf
    int        idx;          // Index into the values in the packet
    int        value;        // RC data in 100s of nanoseconds
    int        hi[MAX_CHAN]; // pulse high times
    int        lo[MAX_CHAN]; // pulse low times 


    pctx = (RCRXDEV *)(pslot->priv);  // Our "private" data

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  // Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.   We get two kinds
    // of packets: gpio read responses and pulse timing.
    if ( ! (   // if not a read response or autosend of timing
            ((pkt->reg == REG_CONFIG) && (pkt->count == 1) &&
              ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA)) ||
            ((pkt->reg == REG_RCDATA) && (pkt->count == 32) &&
              ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA)))) {
        edlog("invalid rcrx packet from board to host");
        return;
    }

    // If a read response from a user dpget command, send value to UI
    prscval = &(pslot->rsc[RSC_GPIOVAL]);
    if ((pkt->reg == REG_CONFIG) && (prscval->uilock != -1)) {
        // gpio values are in bits 6 and 7
        clen = sprintf(cstr, "%1x\n", ((pkt->data[0] >> 6) & MASK_GPIOVAL));
        send_ui(cstr, clen, prscval->uilock);
        prompt(prscval->uilock);

        // Response sent so clear the lock
        prscval->uilock = -1;
        del_timer(pctx->ptimer);  // Got the response
        pctx->ptimer = 0;
        return;
    }

    // Process of elimination makes this an autosend packet.
    // Extract the data and set the pktgood status.  This is
    // used to update the recv status LED.  Only need to get
    // the first 4*nchan bytes from the packet
    for (idx = 0; idx < pctx->nchan; idx++) {
        value = ((pkt->data[4 * idx] << 8) + pkt->data[(4 * idx) + 1]) & 0x7fff;
        hi[idx] = value;  // save high pulse time
        value = ((pkt->data[(4 * idx) + 2] << 8) + pkt->data[(4 * idx) + 3]) & 0x7fff;
        lo[idx] = value;  // same low pulse time
    }

    // If anyone wants the decoded data and if the received packet is good,
    // send it to the user
    prscdat = &(pslot->rsc[RSC_RCDATA]);
    if (prscdat->bkey != 0) {
        clen = 0;
        for (idx = 0; idx < pctx->nchan; idx++) {
            if (idx == pctx->nchan -1)
                ret = sprintf(&(cstr[clen]), "%5d %5d %d\n", hi[idx], lo[idx], (pkt->data[0] >> 7));
            else
                ret = sprintf(&(cstr[clen]), "%5d %5d ", hi[idx], lo[idx]);
            if (ret <= 0) {
                return;    // should never occue
            }
            clen += ret;
        }
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(cstr, clen, &(prscdat->bkey));
        return;
    }

    return;
}


/**************************************************************
 * userparm():  - The user is reading or setting a configuration param
 **************************************************************/
static void userparm(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    RCRXDEV *pctx;     // our local info
    DP_PKT   pkt;      // packet to the FPGA card
    CORE    *pmycore;  // FPGA peripheral info
    int      intval;   // int conversion for nchan/dir/val
    int      ret;      // return scanf/printf status
    int      txret;    // ==0 if the packet went out OK

    pctx = (RCRXDEV *) pslot->priv;
    pmycore = pslot->pcore;

    if (rscid == RSC_NCHAN) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%d\n", pctx->nchan);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%d", &intval);
            if ((ret != 1) || (intval > 8) || (intval < 2)) {
                ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                return;
            }
            pctx->nchan = intval;
        }
    }

    if (rscid == RSC_GPIODIR) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%d\n", pctx->gpiodir);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%d", &intval);
            if ((ret != 1) || (intval > 3) || (intval < 0)) {
                ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                return;
            }
            pctx->gpiodir = intval;
        }
    }

    if (rscid == RSC_GPIOVAL) {
        if (cmd == EDGET) {
            // create a read packet to get the current value of the pins
            pkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
            pkt.core = pmycore->core_id;
            pkt.reg = REG_CONFIG;
            pkt.count = 1;
            // send the packet.  Report any errors
            txret = dpi_tx_pkt(pmycore, &pkt, 4); // header only on read request
            if (txret != 0) {
                ret = snprintf(buf, *plen, E_WRFPGA);
                *plen = ret;  // (errors are handled in calling routine)
                return;
            }               
            // Start timer to look for a read response.
            if (pctx->ptimer == 0)
                pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
            // lock this resource to the UI session cn
            pslot->rsc[RSC_GPIOVAL].uilock = (char) cn;
            // Nothing to send back to the user
            *plen = 0;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%d", &intval);
            if ((ret != 1) || (intval > 3) || (intval < 0)) {
                ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                return;
            }
            pctx->gpioval = intval;
        }
    }

    // a user configurable parameter has changed.  Update the FPGA
    sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send params to the FPGA card. 
 * Put error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    RCRXDEV *pctx,     // This peripheral's context
    int      *plen,    // size of buf on input, #char in buf on output
    char     *buf)     // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the rcrx  
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Write the values for sample rate and which edges to count
    // down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = REG_CONFIG;        // the first reg of the two
    pkt.count = 1;               // 2 data bytes
    pkt.data[0] = (pctx->gpiodir << 6) | (pctx->gpioval << 4) | (pctx->nchan);

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
    RCRXDEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of rcrx  .c
