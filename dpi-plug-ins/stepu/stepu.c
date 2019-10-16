/*
 *  Name: stepu.c
 *
 *  Description: Driver for the unipolar stepper motor peripheral
 *
 *  Resources:
 *    config       - mode, direction, step rate, holding current
 *    count        - how many steps to step
 *    addcount     - synchronously add steps to count
 *
 *  Registers are (high byte)
 *    Addr=0    12 bit target step count, decremented to zero
 *    Addr=2    12 bit value synchronously added to the target, write only
 *    Addr=4    5 bits of setup
 *    Addr=5    8 bits of period
 *    Addr=6    low 7 bits are the holding current PWM value
 *
 *  The setup register has the following bits
 *   Bit 4    on/off     1==on.  All output high for OFF -- brake mode
 *   Bit 3    direction  1==abcd, 0=dcba
 *   Bit 2    half/full  1==half
 *   bit 1,0  00         period clock is 1 microsecond
 *            01         period clock is 10 microseconds
 *            10         period clock is 100 microseconds
 *            11         period clock is 1 millisecond
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
 *  Design notes:
 *    The controller on the FPGA board has four registers.  The
 * first, called "count",  is a read/write 16 bit register with
 * the number of steps remaining until the current move is complete.
 * The maximum step count is 4096, and count is decremented as the
 * motor turns.
 *     A danger when writing directly to the count register is
 * that you might inadvertly remove steps from the target.  That
 * is, if you overwrite a value of 100 with a new value of 300,
 * you've effectively removed 100 steps from the target position.
 * The way around this problem is the second register, which adds
 * steps to the count register without altering the accuracy of
 * the step count.  So to move 8000 steps you would write a value
 * of 4000 to the count register and then some time later do a
 * write of 2000 to the add-steps register, and again later do
 * another write of 2000 to the add-steps register.  This way
 * you are sure to get all 8000 steps.  Timing is important; you
 * need to be sure the count is below 2096 before adding each of
 * the 2000 counts or you'll overflow the count register.  To be
 * sure the count is below 2096 you can poll the count register
 * or set a timer based on the step rate and add more steps when
 * the timer expires.
 *     The holding register is a 7 bit PWM value that specifies
 * how much current to apply once the terminal count has been
 * reached.
 */



#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // STEPU register definitions
#define STEPU_REG_COUNT    0x00
#define STEPU_REG_ADD      0x02
#define STEPU_REG_CNFG     0x04
#define STEPU_REG_PERIOD   0x05
#define STEPU_REG_HOLD     0x06
#define MODE_OFF          'o'
#define MODE_FULL         'f'
#define MODE_HALF         'h'
#define DIR_FORWARD       'f'
#define DIR_REVERSE       'r'
#define MIN_RATE          4
        // resource names and numbers
#define FN_COUNT           "count"
#define FN_ADD             "addcount"
#define FN_CNFG            "config"
#define RSC_COUNT          0
#define RSC_ADD            1
#define RSC_CNFG           2
        // misc defines
#define MAX_LINE_LEN       100       /* max line length from the user */
#define MAXCOUNT           4095      /* 2^12 - 1 */
#define CNTMXLEN           10        /* 12 bit count as an ascii string */
#define MAXRATE            50000     /* maximum step rate */



/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an stepu
typedef struct
{
    void    *pslot;         // handle to peripheral's slot info
    void    *ptimer;        // timer to watch for dropped ACK packets
    char     mode;          // Mode, one of 'o', 'f', 'h' = off, full, half
    char     dir;           // Direction, one of 'f' or 'r' = forward or reverse
    int      rate;          // the actual step rate used internally
    int      period;        // count of clock pulses to get rate at clksrc
    int      scale;         // clock source 0=1MHz, 1=100KHz, 2=10KHz, 3=1KHz
    int      count;         // Most recent target step count from user
    int      addcount;      // User specified value to add to count
    int      holding;       // The holding current as a percentage
} STEPUDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user_hdlr(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, STEPUDEV *);
static void sendconfigtofpga(STEPUDEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    STEPUDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (STEPUDEV *) malloc(sizeof(STEPUDEV));
    if (pctx == (STEPUDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in stepu initialization");
        return (-1);
    }

    // Init our STEPUDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->mode = 'o';          // 'o'ff
    pctx->dir  = 'f';          // 'f'orward
    pctx->holding = 0;         // zero holding current
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->period = 1;
    pctx->scale = 3;           // 1KHz clock
    pctx->rate = 1000;         // rate of period at scale


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_COUNT].name = FN_COUNT;
    pslot->rsc[RSC_COUNT].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_COUNT].bkey = 0;
    pslot->rsc[RSC_COUNT].pgscb = user_hdlr;
    pslot->rsc[RSC_COUNT].uilock = -1;
    pslot->rsc[RSC_COUNT].slot = pslot;
    pslot->rsc[RSC_ADD].name = FN_ADD;
    pslot->rsc[RSC_ADD].flags = IS_WRITABLE;
    pslot->rsc[RSC_ADD].bkey = 0;
    pslot->rsc[RSC_ADD].pgscb = user_hdlr;
    pslot->rsc[RSC_ADD].uilock = -1;
    pslot->rsc[RSC_ADD].slot = pslot;
    pslot->rsc[RSC_CNFG].name = FN_CNFG;
    pslot->rsc[RSC_CNFG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CNFG].bkey = 0;
    pslot->rsc[RSC_CNFG].pgscb = user_hdlr;
    pslot->rsc[RSC_CNFG].uilock = -1;
    pslot->rsc[RSC_CNFG].slot = pslot;
    pslot->name = "stepu";
    pslot->desc = "Bipolar stepper motor controller";
    pslot->help = README;

    // Send the config to the card
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
    STEPUDEV *pctx;    // our local info
    RSC    *prsc;      // pointer to this slot's pins resource
    char    cntstr[CNTMXLEN];  // count value as an ASCII string
    int     cntlen;    // length of count value string

    pctx = (STEPUDEV *)(pslot->priv);  // Our "private" data is a STEPUDEV
    prsc = &(pslot->rsc[RSC_COUNT]);

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the count should come in since we don't ever read the config
    // or holding current PWM value
    if ((pkt->reg != STEPU_REG_COUNT) || (pkt->count != 2) ||
        ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA)) {
        edlog("invalid stepu packet from board to host");
        return;
    }

    // Must be a read response from a user dpget command, send value to UI
    cntlen = snprintf(cntstr, CNTMXLEN, "%d\n", (pkt->data[0] << 8) + pkt->data[1]);
    send_ui(cntstr, cntlen, prsc->uilock);
    prompt(prsc->uilock);

    // Response sent so clear the lock
    prsc->uilock = -1;
    del_timer(pctx->ptimer);  //Got the response
    pctx->ptimer = 0;

    return;
}


/**************************************************************
 * user_hdlr():  - The user is reading or writing resources.
 **************************************************************/
static void user_hdlr(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    STEPUDEV *pctx;    // our local info
    DP_PKT   pkt;      // packet to the FPGA card
    CORE    *pmycore;  // FPGA peripheral info
    int      ret;      // return count
    int      txret;    // ==0 if the packet went out OK
    int      newcount; // new absolute count to send to fpga
    int      newadd;   // new incremental count to send to fpga
    char     newmode[CNTMXLEN]; // new mode of off, full, or half
    char     newdir[CNTMXLEN]; // new direction of forward or reverse
    int      newrate;  // new desired step rate
    int      newhold;  // new percent holding current

    pctx = (STEPUDEV *) pslot->priv;
    pmycore = pslot->pcore;

    if ((cmd == EDGET) && (rscid == RSC_COUNT)) {
        // create a read packet to get the current value of the pins
        pkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = STEPU_REG_COUNT;
        pkt.count = 2;
        // send the packet.  Report any errors
        txret = dpi_tx_pkt(pmycore, &pkt, 4);  // 4 header bytes
        if (txret != 0) {
            ret = snprintf(buf, *plen, E_WRFPGA);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        // Start timer to look for a read response.
        if (pctx->ptimer == 0)
            pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
        // lock this resource to the UI session cn
        pslot->rsc[RSC_COUNT].uilock = (char) cn;
        // Nothing to send back to the user
        *plen = 0;
    }
    if ((cmd == EDGET) && (rscid == RSC_CNFG)) {
        ret = snprintf(buf, *plen, "%s %s %d %d\n",
                    ((pctx->mode == 'o') ? "off" :
                     (pctx->mode == 'f') ? "full" :
                     (pctx->mode == 'h') ? "half" : "illegal mode" ),
                    ((pctx->dir == 'f') ? "forward" :
                     (pctx->dir == 'r') ? "reverse" : "illegal direction" ),
                     pctx->rate, pctx->holding);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDSET) && (rscid == RSC_COUNT)) {
        ret = sscanf(val, "%d", &newcount);
        if ((ret != 1) || (newcount < 0) || (newcount > MAXCOUNT)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->count = newcount;
        pctx->addcount = 0;           // tells sndcfg to send count not addcount
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }
    else if ((cmd == EDSET) && (rscid == RSC_ADD)) {
        ret = sscanf(val, "%d", &newadd);
        if ((ret != 1) || (newadd < 0) || (newadd > MAXCOUNT)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->count = 0;                    // tells sndcfg to send addcount not count
        pctx->addcount = newadd;
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }
    else if ((cmd == EDSET) && (rscid == RSC_CNFG)) {
        ret = sscanf(val, "%5s %8s %d %d", newmode, newdir, &newrate, &newhold);
        newmode[0] = tolower(newmode[0]);
        newdir[0] = tolower(newdir[0]);
        if ((ret != 4) ||
            ((newmode[0] != 'o') && (newmode[0] != 'f') && (newmode[0] != 'h')) ||
            ((newdir[0] != 'f') && (newdir[0] != 'r')) ||
            (newrate < 0) ||
            ((newhold < 0) || (newhold > 100)))
       {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->count = 0;                    // tell sndcfg to send config not count
        pctx->addcount = 0;
        pctx->mode = newmode[0];            // 'o'ff, 'f'ull, or 'h'alf
        pctx->dir  = newdir[0];             // 'f'orward or 'r'everse
        pctx->holding = newhold;            // between 0 and 100
        // Compute a rate that we can do that is close to the requested one
        if (newrate > 1000000) {
            pctx->period = 1;
            pctx->scale = 0;   // microsecond clock source
            pctx->rate = 1000000;
        }
        else if (newrate > 4000) {
            pctx->period = (1000000 / newrate);
            pctx->scale = 0;   // microsecond clock source
            pctx->rate = (1000000 / pctx->period);
        }
        else if (newrate > 400) {
            pctx->period = (100000 / newrate);
            pctx->scale = 1;   // 10 microsecond clock source
            pctx->rate = (100000 / pctx->period);
        }
        else if (newrate > 40) {
            pctx->period = (10000 / newrate);
            pctx->scale = 2;   // 100 microsecond clock source
            pctx->rate = (10000 / pctx->period);
        }
        else if (newrate > 4) {
            pctx->period = (1000 / newrate);
            pctx->scale = 3;   // millisecond clock source
            pctx->rate = (1000 / pctx->period);
        }
        else {
            pctx->period = 250;
            pctx->scale = 3;   // millisecond clock source
            pctx->rate = (1000 / pctx->period);
        }
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }

    return;
}


/**************************************************************
 * sendconfigtofpga():  - Send config or counts to the FPGA.
 * Put error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    STEPUDEV *pctx,    // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the stepu
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // Send count if count != 0
    // Else send addcount if addcount != 0
    // Else send config

    if (pctx->count != 0) {
        pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = STEPU_REG_COUNT;
        pkt.count = 2;
        pkt.data[0] = pctx->count >> 8;
        pkt.data[1] = pctx->count & 0x00ff;
        txret = dpi_tx_pkt(pmycore, &pkt, 6); // 4 header + 2 data
    }
    else if (pctx->addcount != 0) {
        pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = STEPU_REG_ADD;
        pkt.count = 2;
        pkt.data[0] = pctx->addcount >> 8;
        pkt.data[1] = pctx->addcount & 0x00ff;
        txret = dpi_tx_pkt(pmycore, &pkt, 6); // 4 header + 2 data
    }
    else {
        pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
        pkt.core = (pslot->pcore)->core_id;
        pkt.reg = STEPU_REG_CNFG;
        pkt.count = 3;
        /*  The setup register has the following bits
         *   Bit 4    on/off     1==on.  All output high for OFF -- brake mode
         *   Bit 3    direction  1==abcd, 0=dcba
         *   Bit 2    half/full  1==half
         *   bit 1,0  00         period clock is 1 microsecond
         *            01         period clock is 10 microseconds
         *            10         period clock is 100 microseconds
         *            11         period clock is 1 millisecond */
        pkt.data[0]  = (pctx->mode != 'o') ? 0x10 : 0;  // on or off
        pkt.data[0] |= (pctx->dir == 'f') ? 0x08 : 0;   // direction
        pkt.data[0] |= (pctx->mode == 'h') ? 0x04 : 0;  // half or full steps
        pkt.data[0] |= pctx->scale & 0x03;              // clock scaler
        pkt.data[1] = pctx->period;                     // 8 bits of period
        pkt.data[2] = pctx->holding;                    // 7 bits of PWM current
        txret = dpi_tx_pkt(pmycore, &pkt, 7); // 4 header + 3 data
    }

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
    STEPUDEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of stepu.c
