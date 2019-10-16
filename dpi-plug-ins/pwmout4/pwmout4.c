/*
 *  Name: pwmout4.c
 *
 *  Description: Driver for the pgen16 peripheral
 *
 *  The pgen16 is a small pattern generator.  The generator goes through
 *  16 steps where each step is exactly 256 counts long.  Within a step
 *  the outputs are set to that step's value when the count equals that
 *  step's trigger count.
 *      Note that the total period is always 4096 (16*256) counts long.
 *
 *  Hardware Registers:
 *      Registers 0 to 31 are formed into 16 pairs.  The lower register,
 *      0,2,4, ... are the 8 bits of the trigger counter.  The higher
 *      numbered registers, 1,3,5,... are the values to latch for the
 *      outputs when the trigger count is reached in that step.
 *
 *      Reg 32: Clk source in the lower 4 bits
 *
 *  The clock source is selected by the lower 4 bits of register 32:
 *      0:  Off
 *      1:  20 MHz
 *      2:  10 MHz
 *      3:  5 MHz
 *      4:  1 MHz
 *      5:  500 KHz
 *      6:  100 KHz
 *      7:  50 KHz
 *      8:  10 KHz
 *      9   5 KHz
 *     10   1 KHz
 *     11:  500 Hz
 *     12:  100 Hz
 *     13:  50 Hz
 *     14:  10 Hz
 *     15:  5 Hz
 *  Resources:
 *    config    - clock frequency.  Must be one of the above
 *    pwm       - four 12 bit hex values that are the PWM widths
 *
 */

/*
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
        // PWM4 register definitions
#define PWM4_REG_BASE       0     // first pgen16 hardware register
#define PWM4_REG_FREQ       32    // register id for frequency
        // misc constants
#define MAX_LINE_LEN        100
#define NUMPWM              4     // four pwm outputs
        // Resources
#define RSC_CONFIG          0
#define RSC_PWMS            1
        // Slot related defines
#define MAXTM  4097
#define SLOTTM 256

/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an pwm4
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      width[NUMPWM]; // pulse widths in nanoseconds
    int      freq;     // PWM drive frequency
    void    *ptimer;   // timer to watch for dropped ACK packets
} PWM4DEV;

/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void pwm4user(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, PWM4DEV *);
static void pwmtotimes(int [], unsigned char [], unsigned char []);
static int  pwm4tofpga(PWM4DEV *, int rscid);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    PWM4DEV  *pctx;    // our local device context
    int        i;

    // Allocate memory for this peripheral
    pctx = (PWM4DEV *) malloc(sizeof(PWM4DEV));
    if (pctx == (PWM4DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in pwm4 initialization");
        return (-1);
    }

    // Init our PWM4DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    for (i = 0; i < NUMPWM; i++) {
        pctx->width[i] = 0;    // default is off
    }

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_CONFIG].name = "config";
    pslot->rsc[RSC_CONFIG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CONFIG].bkey = 0;
    pslot->rsc[RSC_CONFIG].pgscb = pwm4user;
    pslot->rsc[RSC_CONFIG].uilock = -1;
    pslot->rsc[RSC_CONFIG].slot = pslot;
    pslot->rsc[RSC_PWMS].name = "pwms";
    pslot->rsc[RSC_PWMS].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_PWMS].bkey = 0;
    pslot->rsc[RSC_PWMS].pgscb = pwm4user;
    pslot->rsc[RSC_PWMS].uilock = -1;
    pslot->rsc[RSC_PWMS].slot = pslot;
    pslot->name = "pwmout4";
    pslot->desc = "Quad 12-bit PWM output";
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
    PWM4DEV *pctx;     // our local info

    pctx = (PWM4DEV *)(pslot->priv);  // Our "private" data is a PWM4DEV

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // There are no other packets from the pwm FPGA code so if we
    // get here there is a problem.  Log the error.
    edlog("invalid pwm4 packet from board to host");

    return;
}


/**************************************************************
 * pwm4user():  - The user is reading or writing the pwm widths.
 * Get the value and update the pwm4 on the BaseBoard or read the
 * value and write it into the supplied buffer.
 **************************************************************/
static void pwm4user(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    PWM4DEV *pctx;     // our local info
    int      ret;      // return count
    int      newfreq;  // new value to assign the pwm4
    int      txret;    // ==0 if the packet went out OK
    int      i;
    int      nw[NUMPWM]; // used when reading all pwms from user
    int      notvalid = 0;  // set to 1 if user enters bogus values

    pctx = (PWM4DEV *) pslot->priv;

    // print individual pulse width
    if ((cmd == EDGET) && (rscid == RSC_CONFIG)) {
        ret = snprintf(buf, *plen, "%d\n", pctx->freq);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDGET) && (rscid == RSC_PWMS)) {
        ret = snprintf(buf, *plen, "%4x %4x %4x %4x\n", pctx->width[0],
                 pctx->width[1], pctx->width[2], pctx->width[3]);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDSET) && (rscid == RSC_CONFIG)) {
        ret = sscanf(val, "%d", &newfreq);
        // frequency must be one of the valid values
        if ((ret != 1) || 
            ((newfreq != 20000000) && (newfreq != 10000000) &&
             (newfreq != 5000000) && (newfreq != 1000000) &&
             (newfreq != 500000) && (newfreq != 100000) &&
             (newfreq != 50000) && (newfreq != 10000) &&
             (newfreq != 5000) && (newfreq != 1000) &&
             (newfreq != 500) && (newfreq != 100) &&
             (newfreq != 50) && (newfreq != 10) && (newfreq != 5)))
        {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        // Valid new frequency
        pctx->freq = newfreq;
    }
    else if ((cmd == EDSET) && (rscid == RSC_PWMS)) {
        ret = sscanf(val, "%x %x %x %x", &nw[0], &nw[1], &nw[2], &nw[3]);
        for (i = 0; i < NUMPWM; i++) {
            if (nw[i] > 0x1000)
                notvalid = 1;
        }
        if ((ret != 4) || (notvalid == 1)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        for (i = 0; i < NUMPWM; i++) {
            pctx->width[i] = nw[i];
        }
    }

    txret =  pwm4tofpga(pctx, rscid);   // This peripheral's context
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
 * pwmtotimes():  - This routine arranges the edges of the four
 * bit transitions so that the PWM value are correct.
 **************************************************************/
static void pwmtotimes(
    int width[],                  // desired widths
    unsigned char sttm[],         // transition times
    unsigned char val[])          // values at each transition
{
    int           i, j, k, m;
    int           doneslot[16];       // set to 1 if slow is in use and can't change
    int           dslot;              // PWM width in terms of slots
    int           doffs;              // PWM width in terms of slots
    int           mask[] = { 0x01, 0x02, 0x04, 0x08 };


    // init all working values
    for (i = 0; i< 16; i++) {
        doneslot[i] = 0;
        sttm[i] = 0;
        val[i] = 0;
    }

    // Compute the start and stop times for the four PWM
    // signals.  The done arrays are set to indicate that
    // an output is done or a slot is in use and so the
    // corresponding sttm is fixed.

    // Ignore PWM of zero since that is the default config.
    // PWM of 100% does not require any edges but does set val.
    for (i = 0; i < NUMPWM; i++) {
        if (width[i] == 0) {
            continue;
        }
        else if (width[i] == MAXTM) {
            // set all slots output i to a 1
            for (j = 0; j < 16; j++) {
                val[j] |= mask[i];
            }
            continue;
        }
        // what is the distance between the start and stop in terms of slots
        dslot = width[i] / SLOTTM;
        doffs = width[i] % SLOTTM;
        // find two free slots that are dslot apart
        for (j = 0; j < 16; j++) {
            k = j + dslot;
            k = (dslot == 0) ? k+1 : k;    // must use at least 2 slots
            k = (k < 16) ? k : (k - 16);
            // Are both slots free?
            if ((doneslot[j] == 0) && (doneslot[k] == 0)) {
                doneslot[j] = 1;
                doneslot[k] = 1;
                if (dslot == 0) {
                    sttm[j] = SLOTTM - doffs;
                    val[j] |= mask[i];
                }
                else {
                    sttm[j] = 0;
                    sttm[k] = doffs;
                    for (m = j; m < (j + dslot); m++) {
                        if (m < 16)
                            val[m] |= mask[i];
                        else
                            val[m - 16] |= mask[i]; 
                    }
                }
                break;
            }
        }
    }

    return;
}



/**************************************************************
 * pwm4tofpga():  - Send pwm pulse width to the FPGA card.
 * Return zero on success
 **************************************************************/
static int pwm4tofpga(
    PWM4DEV  *pctx,    // This peripheral's context
    int       rscid)   // Servo number or NUMPWM
{
    DP_PKT   pkt;      // send write and read cmds to the pwm4
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      i;
    unsigned char sttm[16];
    unsigned char val[16];

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // Compute the start and stop times for the four PWM
    // signals.  We pass in width[] and get back an array
    // of 16 start times (within each time slot) and the
    // to write to the outputs at that time.
    pwmtotimes(pctx->width, sttm, val);

    // New pwm pulse widths.  Send down to the card.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = PWM4_REG_BASE;
    pkt.count = 33;
    for (i = 0; i < 16; i++) {
        pkt.data[2 * i] = sttm[i];
        pkt.data[(2*i) + 1] = val[i];
    }

    // map frequency to the four bit constant
    pkt.data[PWM4_REG_FREQ] = 
        (pctx->freq == 20000000) ? 1 :
        (pctx->freq == 10000000) ? 2 :
        (pctx->freq ==  5000000) ? 3 :
        (pctx->freq ==  1000000) ? 4 :
        (pctx->freq ==   500000) ? 5 :
        (pctx->freq ==   100000) ? 6 :
        (pctx->freq ==    50000) ? 7 :
        (pctx->freq ==    10000) ? 8 :
        (pctx->freq ==     5000) ? 9 :
        (pctx->freq ==     1000) ? 10 :
        (pctx->freq ==      500) ? 11 :
        (pctx->freq ==      100) ? 12 :
        (pctx->freq ==       50) ? 13 :
        (pctx->freq ==       10) ? 14 :
        (pctx->freq ==        5) ? 15 : 0;  // zero turns the clock off

    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header +  33 data

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void      *timer,   // handle of the timer that expired
    PWM4DEV *pctx)      // points to instance of this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

