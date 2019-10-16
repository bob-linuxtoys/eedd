/*
 * Name: servo4.c
 *
 * Description: Quad servo peripheral
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
 *    The quad servo motor controller can control up to four servo motors.
 *  Each output has a pulse width between 0 and 2.5 milliseconds.  The
 *  repetition time for all eight servos is 10 milliseconds.
 *    Each servo has a window of 2.5 milliseconds or about fifty thousand
 *  50 ns counts.  The value in the register specifies the 50 ns count at
 *  which the pin goes high.  The pin stays high until the count reaches 2.5
 *  milliseconds.  To get a pulse width of 1.0 ms you would subtract 1.0
 *  from 2.5 giving how long the _low_ time should be.  A low time of 1.5 ms
 *  would give a count of 30000 clock pulses, or a register value of 16'h7530.
 *
 *  The 2.5 ms window for each output is staggered from the previous one.
 *  A waveform for the output might look something like this:
 *     window:  |          |          |          |          |          |
 *     output0: ____|------|____________________________________|------|___
 *     output1: _________________|----|____________________________________
 *     output2: ________________________|--------|_________________________
 *     output3: _________________________________________|--|______________
 *  Please contact Demand Peripherals if you wish for a timing pattern other
 *  than the one presented here.
 *
 *  Registers:
 *    The quad servo controller uses four 16-bit registers for control.
 *    Reg 0:  Servo 0 low pulse width in units of 50 ns.
 *    Reg 2:  Servo 1 low pulse width in units of 50 ns.
 *    Reg 4:  Servo 2 low pulse width in units of 50 ns.
 *    Reg 6:  Servo 3 low pulse width in units of 50 ns.
 *
 *
 * Resources:
 *    The device interfaces to the Quad Servo Controller include individual
 * controls for each servo as well as a control which manipulates the servos
 * as a group.
 *
 * servo0, servo1, servo2, and servo3 : pulse width in nanoseconds
 * servogroup : four space separated pulse widths
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
        // SERVO4 register definitions
#define SERVO4_REG_SERVO0 0     // pulse width count for servo 0
#define SERVO4_REG_SERVO1 2     // pulse width count for servo 1
#define SERVO4_REG_SERVO2 4     // pulse width count for servo 2
#define SERVO4_REG_SERVO3 6     // pulse width count for servo 3
        // misc constants
#define MAX_LINE_LEN        100
        // pulsewidth / count transfer fcts
#define WIDTH_TO_COUNT(p) ((2500000 - (p)) / 50)
        // common positions (change if needed)
#define IGNORED             -1  // ignore in a group
#define POS_MINIMUM         1000000 // minimum pulsewidth in nS
#define POS_CENTERED        1500000 // center pulsewidth in nS
#define POS_MAXIMUM         2000000 // maximum pulsewidth in nS
#define NUMSERVO            4   // number of servos
        // resource names and numbers
#define RSC_GROUP           NUMSERVO
char *servoname[] = {
    "servo1",
    "servo2",
    "servo3",
    "servo4",
};


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an servo4
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      width[NUMSERVO]; // pulse widths in nanoseconds
    void    *ptimer;   // timer to watch for dropped ACK packets
} SERVO4DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void servo4user(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, SERVO4DEV *);
static int  servo4tofpga(SERVO4DEV *, int rscid);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    SERVO4DEV *pctx;    // our local device context
    int        i;

    // Allocate memory for this peripheral
    pctx = (SERVO4DEV *) malloc(sizeof(SERVO4DEV));
    if (pctx == (SERVO4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in servo4 initialization");
        return (-1);
    }

    // Init our SERVO4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    for (i = 0; i < NUMSERVO; i++) {
        pctx->width[i] = POS_CENTERED;
    }

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    for (i = 0; i < NUMSERVO; i++) {
        pslot->rsc[i].name = servoname[i];
        pslot->rsc[i].flags = IS_READABLE | IS_WRITABLE;
        pslot->rsc[i].bkey = 0;
        pslot->rsc[i].pgscb = servo4user;
        pslot->rsc[i].uilock = -1;
        pslot->rsc[i].slot = pslot;
    }
    pslot->rsc[RSC_GROUP].name = "servogroup";
    pslot->rsc[RSC_GROUP].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_GROUP].bkey = 0;
    pslot->rsc[RSC_GROUP].pgscb = servo4user;
    pslot->rsc[RSC_GROUP].uilock = -1;
    pslot->rsc[RSC_GROUP].slot = pslot;
    pslot->name = "servo4";
    pslot->desc = "Four servo control pins";
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
    SERVO4DEV *pctx;    // our local info

    pctx = (SERVO4DEV *)(pslot->priv);  // Our "private" data is a SERVO4DEV

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // There are no other packets from the servo FPGA code so if we
    // get here there is a problem.  Log the error.
    edlog("invalid servo4 packet from board to host");

    return;
}


/**************************************************************
 * servo4user():  - The user is reading or writing the servo widths.
 * Get the value and update the servo4 on the BaseBoard or read the
 * value and write it into the supplied buffer.
 **************************************************************/
static void servo4user(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    SERVO4DEV *pctx;   // our local info
    int      ret;      // return count
    int      newwidth; // new value to assign the servo4
    int      txret;    // ==0 if the packet went out OK
    int      i;
    int      nw[NUMSERVO]; // used when reading all servos from user

    pctx = (SERVO4DEV *) pslot->priv;

    // Note that the resource ID is cleverly arranged to be the
    // index into the widths table.

    // print individual pulse width
    if ((cmd == EDGET) && (rscid < NUMSERVO)) {
        ret = snprintf(buf, *plen, "%d\n", pctx->width[rscid]);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%d %d %d %d\n", pctx->width[0],
                 pctx->width[1], pctx->width[2], pctx->width[3]);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Must be a dpset
    if (rscid < NUMSERVO) {
        ret = sscanf(val, "%d", &newwidth);
        if ((ret != 1) || (newwidth < POS_MINIMUM) || (newwidth > POS_MAXIMUM)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->width[rscid] = newwidth;
    }
    else {
        ret = sscanf(val, "%d %d %d %d", &nw[0], &nw[1], &nw[2], &nw[3]);
        // Verify that we got all four values and that if not -1 they are
        // within the range of valid values.
        for (i = 0; i < NUMSERVO; i++) {
            if (((nw[i] != -1) && (nw[i] < POS_MINIMUM)) || (nw[i] > POS_MAXIMUM)) {
                break;
            }
        }
        if ((ret != 4) || (i < NUMSERVO)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        for (i = 0; i < NUMSERVO; i++) {
            // Ignore new width if it is -1
            pctx->width[i] = (nw[i] == -1) ? pctx->width[i] : nw[i];
        }
    }

    txret =  servo4tofpga(pctx, rscid);   // This peripheral's context
    if (txret != 0) {
        // the send of the new value did not succeed.  This probably
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
 * servo4tofpga():  - Send servo pulse width to the FPGA card.
 * Return zero on success
 **************************************************************/
int servo4tofpga(
    SERVO4DEV *pctx,    // This peripheral's context
    int        rscid)   // Servo number or NUMSERVO
{
    DP_PKT   pkt;      // send write and read cmds to the servo4
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      ontime;   // when in the 2.5 ms window to go high
    int      txret;    // ==0 if the packet went out OK
    int      i;

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // Got a new servo pulse width.  Send down to the card.
    // Build and send the write command to set the servo4
    // based on whether or not is was one servo or all of them.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    if (rscid < NUMSERVO) {
        // '2' is the number of bytes in the pulse width register.
        pkt.reg = SERVO4_REG_SERVO0 + (2 * rscid);
        pkt.count = 2;
        // window is 2.5 ms, resolution is 50 ns
        ontime = ((2500000 - (pctx->width[rscid])) / 50);
        pkt.data[0] = (ontime >> 8) & 0x000000ff;  // high
        pkt.data[1] = ontime & 0x000000ff;         // low
        txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data
    }
    else {
        // send all servo values;
        pkt.reg = SERVO4_REG_SERVO0;
        pkt.count = 2 * NUMSERVO;
        for (i = 0; i < NUMSERVO; i++) {
            ontime = ((2500000 - (pctx->width[i])) / 50);
            pkt.data[i * 2] = (ontime >> 8) & 0x000000ff;  // high
            pkt.data[(i * 2) + 1] = ontime & 0x000000ff;   // low
        }
        txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data
    }

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void      *timer,   // handle of the timer that expired
    SERVO4DEV *pctx)    // points to instance of this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}
// end of servo4.c
