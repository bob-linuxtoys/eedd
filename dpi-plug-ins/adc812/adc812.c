/*
 *  Name: adc812.c
 *
 *  Description:
 * The ADC812 card performs a 12 bit ADC on eight inputs at a rate
 * between 4 HZ and 1 KHz.  Two inputs can be combined to form a
 * differential input for the ADC.  Differential inputs have a
 * 13 bit conversion.   The ADC12 uses the Microchip MCP3304.
 * Please see the datasheet for specifications and part details.
 *
 * The device interfaces to the ADC812 let you specify the sample
 * rate and whether or not an input is differential. The samples
 * appear at a select()'able socket as a space separate list * of
 * eight hex values.
 *
 * Resources:
 * config - sample rate and single/differential configuration
 *    This specifies the sample period in milliseconds and which 
 * inputs are differential.  The sample period is given in decimal
 * and must be between 10 and 256.  The differential inputs are given
 * as an 8 bit hexadecimal number.
 *    If differential bit 0 is set then value 0 reports the signed
 * 12-bit value of input 0 minus input 1.  If bit 1 is set then value
 * 1 reports the value of input 1 minus input0.  This pattern is the
 * same for the other differential pairs, 2/3. 4/5, and 6/7.
 *    This is a read-write resource that works with dpset * and dpget
 * but not dpcat.
 *
 * samples - eight space-separated ADC readings as hexadecimal values
 *    There is one line of output for each set of samples. Single-ended
 * inputs give an 12 bit result and differential inputs give a signed 12
 * bit result.  You can use dpcat with this resource
 */

/*
 * Protocol / Registers:
 *    Addr=0/1  Channel 0 ADC value (high byte in reg 0, low in reg 1)
 *    Addr=2    Channel 1 ADC value
 *    Addr=4    Channel 2 ADC value
 *    Addr=6    Channel 3 ADC value
 *    Addr=8    Channel 4 ADC value
 *    Addr=10   Channel 5 ADC value
 *    Addr=12   Channel 6 ADC value
 *    Addr=14   Channel 7 ADC value
 *    Addr=16   Sample interval
 *    Addr=17   Differential inputs
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
        // ADC812 register definitions
#define ADC812_REG_ADC0   0x00
#define ADC812_REG_CNFG   0x10

        // resource names and numbers
#define FN_CONFIG          "config"
#define FN_SAMPLES         "samples"
#define RSC_CONFIG         0
#define RSC_SAMPLES        1
        // Output string len = 8 * ("0x1234 ") - trailing space + newline
#define VALLEN             100
#define VALFMT             "%04x %04x %04x %04x %04x %04x %04x %04x\n"


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an adc812
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      period;   // ADC sample period in milliseconds (1 to 256)
    int      differ;   // 8 bits to specify which inputs are differential
    void    *ptimer;   // timer to watch for dropped ACK packets
} ADC812DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userconfig(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, ADC812DEV *);
static void sendconfigtofpga(ADC812DEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    ADC812DEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (ADC812DEV *) malloc(sizeof(ADC812DEV));
    if (pctx == (ADC812DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in adc812 initialization");
        return (-1);
    }

    // Init our ADC812DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->period = 250;        // default milliseconds per sample
    pctx->differ = 0;          // all inputs are single-ended by default
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_CONFIG].name = FN_CONFIG;
    pslot->rsc[RSC_CONFIG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CONFIG].bkey = 0;
    pslot->rsc[RSC_CONFIG].pgscb = userconfig;
    pslot->rsc[RSC_CONFIG].uilock = -1;
    pslot->rsc[RSC_CONFIG].slot = pslot;
    pslot->rsc[RSC_SAMPLES].name = FN_SAMPLES;
    pslot->rsc[RSC_SAMPLES].flags = CAN_BROADCAST;
    pslot->rsc[RSC_SAMPLES].bkey = 0;
    pslot->rsc[RSC_SAMPLES].pgscb = 0;     // no get/set callback
    pslot->rsc[RSC_SAMPLES].uilock = -1;
    pslot->rsc[RSC_SAMPLES].slot = pslot;
    pslot->name = "adc812";
    pslot->desc = "Octal 12-bit Analog-to-Digital converter";
    pslot->help = README;


    // Send the sample rate and sigle/differential configuration to FPGA.
    // Ignore return value since there's no user connection and
    // system errors are sent to the logger.
    sendconfigtofpga(pctx, (int *) 0, (char *) 0);  // send config

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
    ADC812DEV *pctx;   // our local info
    RSC    *prsc;      // pointer to this slot's samples resource
    char    valstr[VALLEN];  // adc values as space separated string
    int     slen;      // length of value string (should be 47) 

    pctx = (ADC812DEV *)(pslot->priv);  // Our "private" data is a ADC812DEV
    prsc = &(pslot->rsc[RSC_SAMPLES]);

    // Clear the timer on config write response packet
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
        return;
    }

    // Do a sanity check on the received packet.  Only reads from
    // the samples should come in since we don't ever read the period
    // or single/differential registers.
    if ((pkt->reg != ADC812_REG_ADC0) || (pkt->count != 16)) {
        edlog("invalid adc812 packet from board to host");
        return;
    }

    // Process of elimination makes this an autosend packet.
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey != 0) {
        slen = sprintf(valstr, VALFMT,
                  ((pkt->data[0] << 8) + pkt->data[1]),
                  ((pkt->data[2] << 8) + pkt->data[3]),
                  ((pkt->data[4] << 8) + pkt->data[5]),
                  ((pkt->data[6] << 8) + pkt->data[7]),
                  ((pkt->data[8] << 8) + pkt->data[9]),
                  ((pkt->data[10] << 8) + pkt->data[11]),
                  ((pkt->data[12] << 8) + pkt->data[13]),
                  ((pkt->data[14] << 8) + pkt->data[15]));
        send_ui(valstr, slen, prsc->uilock);
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(valstr, slen, &(prsc->bkey));
        return;
    }

    return;
}


/**************************************************************
 * userconfig():  - The user is reading or setting the configuration.
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
    ADC812DEV *pctx;   // our local info
    int      ret;      // return count
    int      newperiod; // new value to assign the period
    int      newdiffer; // new value to assign the differential config

    pctx = (ADC812DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%d 0x%02x\n", pctx->period, pctx->differ);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%d %x", &newperiod, &newdiffer);
        if ((ret != 2) || (newperiod < 10) || (newperiod > 256)) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->period = newperiod;
        pctx->differ = newdiffer;
        sendconfigtofpga(pctx, plen, buf);  // send period, differential config
    }

    return;
}


/**************************************************************
 * sendconfigto():  - Send config values to the FPGA card.
 * Put error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    ADC812DEV *pctx,   // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the adc812
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value

    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // create a write packet to set the mode reg
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = ADC812_REG_CNFG;
    pkt.count = 2;
    pkt.data[0] = pctx->period - 1;   // period is zero-indexed in the hardware 
    pkt.data[1] = pctx->differ;

    // try to send the packet.  Apply or release flow control.
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    if (txret != 0) {
        // the send of the new config did not succeed.  This
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
    void      *timer,   // handle of the timer that expired
    ADC812DEV *pctx)    // this peripheral's context
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of adc812.c
