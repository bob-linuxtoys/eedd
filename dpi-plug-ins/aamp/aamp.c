/*
 *  Name: aamp.c
 *
 *  Description: Device driver for the aamp peripheral
 *
 *  Note: This driver uses the generic out4 peripheral.
 *
 *  Hardware Registers:
 *    0: outval    - 8-bit read/write
 * 
 *  Resources:
 *    volume    - eight bit volume (1-32)
 *    enabled   - enabled state flag, on (1) or off (0)
 *    led       - LED state flag, on (1) or off (0)
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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/

// OUT4 register definitions
#define OUT4_REG_OUTVAL     0x00

// AAMP input bit positions on out4
#define SHFT_ENABLED           0
#define SHFT_VOL_UP         1
#define SHFT_VOL_DOWN       2
#define SHFT_LED            3

// AAMP resource definitions and indices
#define FN_VOLUME           "volume"
#define FN_ENABLED          "enabled"
#define FN_LED              "led"
#define RSC_VOLUME          0
#define RSC_ENABLED         1
#define RSC_LED             2
#define STATE_OFF           0
#define STATE_ON            1
#define VOLUME_CHANGE_DELAY 100

/**************************************************************
 *  - Data structures
 **************************************************************/
 
// All state info for an instance of an aamp
typedef struct
{
    void    *volTimer;      // ==1 means there's a volume timer
    int      volume;        // volume value
    int      pulseLevel;    // the level of the volume pulse to write
    int      targetVolume;  // target volume
    int      enabledState;  // enabled state either on (1) or off (0)
    int      ledState;      // LED state either on (1) or off (0)
    void    *pslot;         // handle to peripheral's slot info
    void    *ptimer;        // timer to watch for dropped ACK packets
} AAMPDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void aampuser(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, AAMPDEV *);
static void VolumeToFpga(void *, AAMPDEV *);
static int  StateFlagsToFpga(AAMPDEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);

/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)    // points to the SLOT for this peripheral
{
    AAMPDEV *pctx;  // our local device context

    // Allocate memory for this peripheral
    pctx = (AAMPDEV *) malloc(sizeof(AAMPDEV));
    if (pctx == (AAMPDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in aamp initialization");
        return (-1);
    }

    // Init our AAMPDEV structure
    pctx->pslot = pslot;            // our instance of a peripheral
    pctx->ptimer = 0;               // set while waiting for a response
    pctx->volume = 32;              // assume power-on volume is highest
    pctx->targetVolume = 1;         // default target is lowest
    pctx->enabledState = STATE_ON;  // amp must be enabled to init volume
    pctx->ledState = STATE_OFF;     // LED is off by default
    pctx->pulseLevel = 1;           // default to high for active low pulse

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb = packet_hdlr;
    pslot->priv = pctx;
    
    // Add the handlers for the user visible resources
    pslot->rsc[RSC_VOLUME].name = FN_VOLUME;
    pslot->rsc[RSC_VOLUME].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_VOLUME].bkey = 0;
    pslot->rsc[RSC_VOLUME].pgscb = aampuser;
    pslot->rsc[RSC_VOLUME].uilock = -1;
    pslot->rsc[RSC_VOLUME].slot = pslot;
    pslot->rsc[RSC_ENABLED].name = FN_ENABLED;
    pslot->rsc[RSC_ENABLED].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_ENABLED].bkey = 0;
    pslot->rsc[RSC_ENABLED].pgscb = aampuser;
    pslot->rsc[RSC_ENABLED].uilock = -1;
    pslot->rsc[RSC_ENABLED].slot = pslot;
    pslot->rsc[RSC_LED].name = FN_LED;
    pslot->rsc[RSC_LED].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_LED].bkey = 0;
    pslot->rsc[RSC_LED].pgscb = aampuser;
    pslot->rsc[RSC_LED].uilock = -1;
    pslot->rsc[RSC_LED].slot = pslot;
    pslot->name = "aamp";
    pslot->desc = "Simple audio amplifier";
    pslot->help = README;
    
    // kick off a volume change which will also init the enabled and LED states
    pctx->volTimer = add_timer(ED_PERIODIC, VOLUME_CHANGE_DELAY, VolumeToFpga, (void *) pctx);

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
    AAMPDEV *pctx;      // our local info

    pctx = (AAMPDEV *)(pslot->priv);  // Our "private" data is a AAMPDEV

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }
    
    // Error if not a write response packet
    else {
        edlog("invalid aamp packet from board to host");
        return;
    }

    return;
}


/**************************************************************
 * aampuser():  - The user is reading or writing the peripheral
 * Get the value and update the aamp on the BaseBoard or read the
 * note and write it into the circular queue of notes to send.
 **************************************************************/
static void aampuser(
    int      cmd,               // ==EDGET if a read, ==DPSET on write
    int      rscid,             // ID of resource being accessed
    char    *val,               // new value for the resource
    SLOT    *pslot,             // pointer to slot info.
    int      cn,                // Index into UI table for requesting conn
    int     *plen,              // size of buf on input, #char in buf on output
    char    *buf)
{
    AAMPDEV *pctx;              // our local info
    int      ret;               // return count
    int      newVolume;         // new value to assign the volume
    int      newEnabledState;   // new enabled state
    int      newLedState;       // new LED state
    int      txret;             // ==0 if the packet went out OK
    
    pctx = (AAMPDEV*)pslot->priv; // our local info

    if (rscid == RSC_VOLUME) 
    {
        if (cmd == EDGET) 
        {
            ret = snprintf(buf, *plen, "%d\n", pctx->volume);
            *plen = ret;  // (errors are handled in calling routine)
        } 
        else 
        {
            // volume changes can only occur if the amp is enabled
            if (pctx->enabledState == STATE_ON)
            {
                ret = sscanf(val, "%d", &newVolume);
                if ((ret != 1) || (newVolume < 1) || (newVolume > 32)) 
                {
                    ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                    *plen = ret;
                    return;
                }
                pctx->targetVolume = newVolume;
                
                // set the new volume of the amp with the use of a periodic timer 
                // with a callback function that increments the volume up or down 
                // from the current volume to the target volume
                if (pctx->volTimer == 0)
                {
                    pctx->volTimer = add_timer(ED_PERIODIC, 100, VolumeToFpga, (void *) pctx);
                }
            }
        }
        return;
    }

    if (rscid == RSC_ENABLED) 
    {
        if (cmd == EDGET) 
        {
            ret = snprintf(buf, *plen, "%d\n", pctx->enabledState);
            *plen = ret;  // (errors are handled in calling routine)
        } 
        else 
        {
            ret = sscanf(val, "%d", &newEnabledState);
            if ((ret != 1) || (newEnabledState < 0) || (newEnabledState > 1)) 
            {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->enabledState = newEnabledState;
            
            // Got new value.  Send it to the FPGA
            txret =  StateFlagsToFpga(pctx);
            if (txret != 0) 
            {
                // the send of the new value did not succeed.  This probably
                // means the input buffer to the USB port is full.  Tell the
                // user of the problem.
                ret = snprintf(buf, *plen, E_WRFPGA);
                *plen = ret;  // (errors are handled in calling routine)
                return;
            }
        }
        return;
    }

    if (rscid == RSC_LED) 
    {
        if (cmd == EDGET) 
        {
            ret = snprintf(buf, *plen, "%d\n", pctx->ledState);
            *plen = ret;  // (errors are handled in calling routine)
        } 
        else 
        {
            ret = sscanf(val, "%x", &newLedState);
            if ((ret != 1) || (newLedState < 0) || (newLedState > 0xff)) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->ledState = newLedState;
            
            // Got new value.  Send it to the FPGA
            txret =  StateFlagsToFpga(pctx);
            if (txret != 0) 
            {
                // the send of the new value did not succeed.  This probably
                // means the input buffer to the USB port is full.  Tell the
                // user of the problem.
                ret = snprintf(buf, *plen, E_WRFPGA);
                *plen = ret;  // (errors are handled in calling routine)
                return;
            }
        }
        return;
    }

    return;
}


/**************************************************************
 * VolumeToFpga():  - This is the callback for the volume 
 *   changing timer.  It will be called to nudge the volume
 *   up or down by one increment at a time until the target 
 *   volume it reached.  Send the next logic level to the correct 
 *   input of the amp to either increment or decrement the 
 *   volume.  The context will define which direction by comparing 
 *   the current volume with the target.  This function sends
 *   either the high or low component of the volume change
 *   pulse.  The timer is deleted when the volume has reached the
 *   target.  
 **************************************************************/
static void VolumeToFpga(void *timer, AAMPDEV *pctx)
{
    DP_PKT   pkt;      // send write and read cmds to the out4
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info

    pmyslot = pctx->pslot;
    pmycore = (CORE *)pmyslot->pcore;

    if (pctx->volume != pctx->targetVolume)
    {
        // Build and send the write command to set the out4.
        pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
        pkt.core = pmycore->core_id;
        pkt.reg = OUT4_REG_OUTVAL;
        pkt.count = 1;
        
        // initialize a 4-bit value with the current LED and enabled state
        pkt.data[0] = 0;
        if (pctx->ledState == STATE_ON)
        {
            // the LED resource is set to 1 for on and 0 for off,
            // which is the same as the signals that must be written
            // to the card to turn the LED on and off
            pkt.data[0] |= (1 << SHFT_LED);
        }
        if (pctx->enabledState == STATE_ON)
        {
            // the enabled resource value and the actual input to the 
            // amplifier device are inverted.  That is, when 1 is 
            // written to the enabled resource (signifying that enabled is
            // "on") a low signal must be written to the device.  
            // Conversely, when 0 is written to the enabled resource
            // (signifying that enabled is "off") a high signal must be 
            // written to the device
            pkt.data[0] |= (1 << SHFT_ENABLED);
        }
        
        // determine whether to increment or decrement the volume
        if (pctx->volume < pctx->targetVolume)
        {
            // modify the 4-value to increment the volume
            pkt.data[0] |= (pctx->pulseLevel << SHFT_VOL_UP);
            pkt.data[0] |= (1 << SHFT_VOL_DOWN);
            if (pctx->pulseLevel == 1)
            {
                // adjust the volume after the complete pulse is written
                pctx->volume++;
            }
        }
        else
        {
            // modify the 4-value to decrement the volume
            pkt.data[0] |= (pctx->pulseLevel << SHFT_VOL_DOWN);
            pkt.data[0] |= (1 << SHFT_VOL_UP);
            if (pctx->pulseLevel == 1)
            {
                // adjust the volume after the complete pulse is written
                pctx->volume--;
            }
        }
        
        // transmit the packet and check for a write response
        dpi_tx_pkt(pmycore, &pkt, 5);
        if (pctx->ptimer == 0)
        {
            pctx->ptimer = add_timer(ED_ONESHOT, VOLUME_CHANGE_DELAY, noAck, (void *) pctx);
        }
        
        // invert the next level of the pulse to write
        pctx->pulseLevel = (pctx->pulseLevel == 0) ? 1 : 0;
        
    }
    else
    {
        // delete the timer
        del_timer(timer);
        pctx->volTimer = 0;
    }
}

/**************************************************************
 * StateFlagsToFpga():  - Send enabled and LED states to the FPGA 
 * card.  Return zero on success.
 **************************************************************/
static int StateFlagsToFpga(
    AAMPDEV *pctx)    // This peripheral's context
{
    DP_PKT   pkt;      // send write and read cmds to the aamp
    SLOT    *pmyslot;  // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK

    pmyslot = pctx->pslot;
    pmycore = (CORE *)pmyslot->pcore;

    // Got a new state for one of the static states, either enabled
    // or LED.  Build and send the write command to set the out4.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = OUT4_REG_OUTVAL;
    pkt.count = 1;
    
    // compose and write a 4-bit value of the LED state, enabled state, 
    // and ones for volume up and down
    pkt.data[0] = 0;
    if (pctx->ledState == STATE_ON)
    {
        // the LED resource is set to 1 for on and 0 for off,
        // which is the same as the signals that must be written
        // to the card to turn the LED on and off
        pkt.data[0] |= (1 << SHFT_LED);
    }
    if (pctx->enabledState == STATE_ON)
    {
        // the enabled resource value and the actual input to the 
        // amplifier device are inverted.  That is, when 1 is 
        // written to the enabled resource (signifying that enabled is
        // "on") a low signal must be written to the device.  
        // Conversely, when 0 is written to the enabled resource
        // (signifying that enabled is "off") a high signal must be 
        // written to the device
        pkt.data[0] |= (1 << SHFT_ENABLED);
    }
    pkt.data[0] |= (1 << SHFT_VOL_UP);
    pkt.data[0] |= (1 << SHFT_VOL_DOWN);
    
    // send the packet and start a timer to look for a write response
    txret = dpi_tx_pkt(pmycore, &pkt, 5);
    if (pctx->ptimer == 0)
    {
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
    }

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void    *timer,   // handle of the timer that expired
    AAMPDEV *pctx)    // points to instance of this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of aamp.c


