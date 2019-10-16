/*
 *  Name: tif.c
 *
 *  Description: Driver for the Text Interface Card
 *    The TIF card provides an interface to a two or four line text LCD, to
 *    a 4x5 keypad, a rotary encoder with button, a piezo beeper, two LEDs,
 *    and the LED backlight of the LCD,  
 *
 *  Hardware Registers: 8 bit, read-write
 *      Reg 0:  Keypad status: 0x00 if no key pressed
 *              Bits 0-2: row of keypress. 
 *              Bits: 3-4: column of keypress.
 *              Bit: 6: not used
 *              Bit: 7: button status on rotary encoder (1==closed)
 *      Reg 1:  Bits 0-3: signed number of rotary pulses.
 *              Bits 6-7: not used
 *      Reg 2:  Bits 0-4: tone duration in units of 10ms
 *              Bits 5-6: note frequency (1454Hz, 726, 363, 181)
 *              Bits 5-7: set for a slightly louder sound
 *      Reg 3:  Bits 0-2: LED control 
 *              Bits 4-7: Contrast control
 *      Reg 4:  Bits 0-4: Character FIFO for the text display.
 *              The low nibble is written first, then the high nibble with the
 *              command/data flag in bit 4.  The peripheral has a 15 character
 *              FIFO.  Set bit 7 to latch the low and high nibbles into the FPGA
 *              FIFO.  If you write more than 15 characters the write response
 *              will tell how many character were not written.  That is, a write
 *              response of zero means all characters were put in the FIFO.
 * 
 *  Resources:
 *    keypad       - read/broadcast keypad state or transitions
 *    encoder      - signed rotary count and center button status (1==closed)
 *    tone         - space separated triple of duration (milliseconds, up to
 *                   310), which note to play (1==1454Hz, 2==726, 3==494, 3==363),
 *                   and a 0 or 1 to slightly increase the sound volume
 *    leds         - a single digit in the range of 0 to 7 to turn on or off
 *                   the LEDs.  Bit 0 is the LCD backlight.  Bit 1 is the User 1
 *                   LED, and Bit 2 is the User 2 LED.
 *    text         - Printable ASCII characters to send to the display.
 *    commands     - Commands as a string of two digit hex characters.  The
 *                   R/S flag on the device is clear when sending commands.
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
#include <ctype.h>
#include "eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // TIF register definitions
#define TIF_R_KEYPAD     0x00
#define TIF_R_ROTARY     0x01
#define TIF_R_TONE       0x02
#define TIF_R_LED        0x03    // LED
#define TIF_R_FIFO       0x04    // cmds/text to the display
        // resource names and numbers
#define FN_KEYPAD          "keypad"
#define FN_ENCODER         "encoder"
#define FN_TONE            "tone"
#define FN_LEDS            "leds"
#define FN_TEXT            "text"
#define FN_COMMAND         "commands"
#define RSC_KEYPAD         0
#define RSC_ENCODER        1
#define RSC_TONE           2
#define RSC_LEDS           3
#define RSC_TEXT           4
#define RSC_COMMAND        5
        // Max length of string to UI
#define TIFMXSTR           20
        // Size of our local FIFO for text and commands
#define FIFOSZ             120


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of a tif peripheral
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    int      keys;     // keypad status.  -1 if no key is down
    int      count;    // most recent rotary count.  negative==CCW
    int      button;   // center button on the rotary encoder, 1==closed
    int      leds;     // user LEDs and the LCD backlight
    int      dur;      // tone duration
    int      tone;     // which of four frequencies to generate
    int      vol;      // one bit volume control
    // We want a local FIFO to augment the 15 character FIFO in the FPGA.
    // Note that this an int array to better accommodate the 9 bit chars
    // that are actually sent to the LCD display,
    int      fifo[FIFOSZ]; // local fifo
    int      idx;      // number of chars in fifo
    int      pending;  // FIFO write response pending if set
} TIFDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void user_hdlr(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *timer, TIFDEV *);
static void sendtofpga(int rscid, TIFDEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    TIFDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (TIFDEV *) malloc(sizeof(TIFDEV));
    if (pctx == (TIFDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in tif initialization");
        return (-1);
    }

    // Init our TIFDEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->keys = 0;            // assume no key down
    pctx->count = 0;           // no movement
    pctx->button = 0;          // center button not pressed
    pctx->leds = 1;            // backlight on, others off
    pctx->idx = 0;             // local LCD FIFO is empty


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_KEYPAD].name = FN_KEYPAD;
    pslot->rsc[RSC_KEYPAD].flags = CAN_BROADCAST;
    pslot->rsc[RSC_KEYPAD].bkey = 0;
    pslot->rsc[RSC_KEYPAD].pgscb = user_hdlr;
    pslot->rsc[RSC_KEYPAD].uilock = -1;
    pslot->rsc[RSC_KEYPAD].slot = pslot;
    pslot->rsc[RSC_ENCODER].name = FN_ENCODER;
    pslot->rsc[RSC_ENCODER].flags = CAN_BROADCAST;
    pslot->rsc[RSC_ENCODER].bkey = 0;
    pslot->rsc[RSC_ENCODER].pgscb = user_hdlr;
    pslot->rsc[RSC_ENCODER].uilock = -1;
    pslot->rsc[RSC_ENCODER].slot = pslot;
    pslot->rsc[RSC_TONE].name = FN_TONE;
    pslot->rsc[RSC_TONE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_TONE].bkey = 0;
    pslot->rsc[RSC_TONE].pgscb = user_hdlr;
    pslot->rsc[RSC_TONE].uilock = -1;
    pslot->rsc[RSC_TONE].slot = pslot;
    pslot->rsc[RSC_LEDS].name = FN_LEDS;
    pslot->rsc[RSC_LEDS].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_LEDS].bkey = 0;
    pslot->rsc[RSC_LEDS].pgscb = user_hdlr;
    pslot->rsc[RSC_LEDS].uilock = -1;
    pslot->rsc[RSC_LEDS].slot = pslot;
    pslot->rsc[RSC_TEXT].name = FN_TEXT;
    pslot->rsc[RSC_TEXT].flags = IS_WRITABLE;
    pslot->rsc[RSC_TEXT].bkey = 0;
    pslot->rsc[RSC_TEXT].pgscb = user_hdlr;
    pslot->rsc[RSC_TEXT].uilock = -1;
    pslot->rsc[RSC_TEXT].slot = pslot;
    pslot->rsc[RSC_COMMAND].name = FN_COMMAND;
    pslot->rsc[RSC_COMMAND].flags = IS_WRITABLE;
    pslot->rsc[RSC_COMMAND].bkey = 0;
    pslot->rsc[RSC_COMMAND].pgscb = user_hdlr;
    pslot->rsc[RSC_COMMAND].uilock = -1;
    pslot->rsc[RSC_COMMAND].slot = pslot;

    pslot->name = "tif";
    pslot->desc = "Text Interface";
    pslot->help = README;

    // Send the LEDs to the card
    sendtofpga(RSC_LEDS, pctx, (int *) 0, (char *) 0);  // send leds

    // Set up an initialization sequence for next LCD send
    pctx->fifo[0] = 0x38;
    pctx->fifo[1] = 0x0e;
    pctx->fifo[2] = 0x01;
    pctx->idx = 3;
    sendtofpga(RSC_TEXT, pctx, (int *) 0, (char *) 0);  // send lcd init
    pctx->pending = 1;

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT    *pslot,    // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    TIFDEV *pctx;      // our local context
    RSC    *pkrsc;     // pointer to this slot's keypad resource
    RSC    *prrsc;     // pointer to this slot's rotary encoder resource
    char    str[TIFMXSTR];  // holds data to broadcast
    int     bcstlen;   // length of str to broadcast
    int     rcount;    // number of chars ACKed
    int     i,j;       // generic loop counters

    /* We expect two kinds of packets from the host:
     * - read response: always of the first two registers and only of the
     *                  first two register but may autosend or not
     * - write response: to acknowledge write to the tone generator, the
     *                  LEDs, or the LCD FIFO.  A write to the FIFO may
     *                  return indicating that not all characters were
     *                  written.  When then happens resend the characters
     *                  that did not make into the FIFO on the first try.
     */

    pctx = (TIFDEV *)(pslot->priv);  // Our "private" data is a TIFDEV

    // Handle read responses first
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_READ) {
        // Sanity check
        if ((pkt->reg != TIF_R_KEYPAD) || (pkt->count != 2)) {
            edlog("invalid tif read response from board to host");
            return;
        }

        // Pull data out of packet and put in our private data struct
        // As binary the keys are ccrrr where cc is 0 to 3 and rrr is
        // 3 to 7.  We want the high nibble to be the column as 1 to 4
        // and the low nibble to be row as 1 to 5.
        if ((pkt->data[0] & 0x1f) != 0) {
            pctx->keys = ((pkt->data[0] & 0x18) << 1) + 0x10;  // column
            pctx->keys += (pkt->data[0] & 0x07) - 2;           // row
        }
        else {
            pctx->keys = 0;
        }
        pctx->button = (pkt->data[0] & 0x80) >> 7;
        pctx->count = pkt->data[1] & 0x07;  // unsigned
        if (pkt->data[1] & 0x08)   // convert from one complement
            pctx->count = -(1 << 3) + pctx->count;

        // Broadcast keypad if any UIs are monitoring it.
        pkrsc = &(pslot->rsc[RSC_KEYPAD]);
        if (pkrsc->bkey != 0) {
            bcstlen = snprintf(str, (TIFMXSTR-1), "%02x\n", pctx->keys);
            // bkey will return cleared if UIs are no longer monitoring us
            bcst_ui(str, bcstlen, &(pkrsc->bkey));
        }
        // Broadcast encoder data if any UIs are monitoring it.
        prrsc = &(pslot->rsc[RSC_ENCODER]);
        if (prrsc->bkey != 0) {
            bcstlen = snprintf(str, (TIFMXSTR-1), "%d %d\n", pctx->count,
                               pctx->button);
            // bkey will return cleared if UIs are no longer monitoring us
            bcst_ui(str, bcstlen, &(prrsc->bkey));
        }
        return;  // done handling read response packet
    }


    // Clear the timer on write response packets
    if ((pctx->ptimer) && ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE)) {
        del_timer(pctx->ptimer);  //Got the ACK
        pctx->ptimer = 0;
    }

    // if this is an ACK to a FIFO write then we want to remove the
    // sent characters from the FIFO.  If there are still more chars
    // in the FIFO we also want to send another packet to the FPGA
    if (pkt->reg == TIF_R_FIFO) {
        pctx->pending = 0;    // clear pending flag
        // If all chars sent then just reset fifo index, clear pending.
        // Recall that write responses have a "write remaining" byte (data[0])
        // to say how many bytes did not get into the FIFO.
        rcount = pkt->data[0] / 2;   // 2 packet bytes per 9-bit character
        if (rcount > pctx->idx) {  // more than sent?????
            edlog("invalid tif fifo write response from board to host");
            return;
        }
        else if ((rcount == 0) && (pctx->idx == (pkt->count / 2))) {
            pctx->idx = 0;
            return;
        }
        // else we have to move the remaining char down and send another pkt.
        // We have to move from (pkt->count -rcount) down to 0 for (idx - pkt-
        for (i = 0, j = ((pkt->count / 2) - rcount); j < pctx->idx; i++, j++) {
            pctx->fifo[i] = pctx->fifo[j];
        }
        pctx->idx = pctx->idx - (pkt->count / 2) + rcount;
    }

    // Send another packet if there are still chars in the buffer
    if ((pctx->idx != 0) && (pctx->pending == 0)) {
        sendtofpga(RSC_TEXT, pctx, (int *) 0, (char *) 0);
        pctx->pending = 1;
    }

    return;
}


/**************************************************************
 * user_hdlr():  - The user is reading or setting a resource
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
    TIFDEV *pctx;      // context for this peripheral instance
    int      ret;      // return count
    int      vallen;   // number of characters in val
    int      tmp1, tmp2, tmp3;     // temporary ints to help conversion
    int      i;        // generic loop variables
    int      tidx;     // temporary index into the text fifo

    pctx = (TIFDEV *) pslot->priv;

    // Possible UI is for LEDs, tone, text, or commands
    if (rscid == RSC_LEDS) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%1x\n", pctx->leds);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%x", &tmp1);
            if ((ret != 1) || (tmp1 < 0) || (tmp1 > 0x7)) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->leds = tmp1;
            sendtofpga(RSC_LEDS, pctx, plen, buf);  // send LEDs
            return;
        }
    }
    else if (rscid == RSC_TONE) {
        if (cmd == EDGET) {
            ret = snprintf(buf, *plen, "%d %d %d\n", pctx->dur, pctx->tone,
                           pctx->vol);
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        else if (cmd == EDSET) {
            ret = sscanf(val, "%d %d %d", &tmp1, &tmp2, &tmp3);
            if ((ret != 3) ||                  // must get all three values
                (tmp1 < 0) ||                  // duration must be positive
                ((tmp2 < 1) && (tmp2 > 4)) ||  // tone is between 1 and 4
                ((tmp3 != 0) && (tmp3 != 1)))  // volume is 0 or 1
            {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->dur = tmp1;
            pctx->tone = tmp2;
            pctx->vol = tmp3;
            sendtofpga(RSC_TONE, pctx, plen, buf);  // send new tone to FPGA
            return;
        }
    }
    else if (rscid == RSC_TEXT) {
        // We want to accept the whole message from the user or none of it.
        // So we make temporary copies of the fifo index and add the message
        // to end of the fifo.  If it fits we just update the real fifo index
        tidx = pctx->idx;
        vallen = strlen(val);

        // copy string to end of fifo
        for (i = 0; i < vallen; i++) {
            // copy char as int and set the R/S flag to 1
            pctx->fifo[tidx++] = (int)val[i] | 0x0100;
            // Abort copy if we hit the end of the FIFO
            if (tidx >= FIFOSZ) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
        }
        // Message fit, so copy temp values to our state
        pctx->idx = tidx;
        // send local fifo to TIF peripheral in FPGA 
        if (pctx->pending == 0) {  // send only if no pkts in transit
            sendtofpga(RSC_TEXT, pctx, plen, buf);  // send text/cmds to FPGA
            pctx->pending = 1;
        }
    }
    else if (rscid == RSC_COMMAND) {
        // Add hex values from user to our local FIFO and send it
        // Since we aren't told how many 2 digit hex character are in val
        // we have to walk the list extracting them one at a time.
        // List should be of the form "xx xx xx xx xx ...".
        vallen = strlen(val);
        i = 0;         // index into val
        tidx = pctx->idx; // number of characters ready to send
        while (1) {
            // error out if needed
            if (tidx == FIFOSZ) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            pctx->fifo[tidx] = 0;
            while (i < vallen) {
                if (isxdigit(val[i]))
                    break;
                i++;
            }
            // i now points at a hex char
            while (i < vallen) {
                tmp1 = (isdigit(val[i])) ? (int)(val[i] - '0') : 
                                           (int)(toupper(val[i]) - 'A' + 10);
                // add hex digit to low 4 bits
                pctx->fifo[tidx] = (pctx->fifo[tidx] << 4) + tmp1;
                i++;
                if (! isxdigit(val[i]))
                    break;
            }
            // At this point we have successfully gotten a hex value in fifo[tidx].
            // Break out of the loop if needed or get next hex character
            tidx++;    // char is added to FIFO.  Increment count
            if (i == vallen)
                break;
        }
        pctx->idx = tidx;

        // send local fifo to TIF peripheral in FPGA
        if (pctx->pending == 0) {  // send only if no pkts in transit
            sendtofpga(RSC_TEXT, pctx, plen, buf);  // send text/cmds to FPGA
            pctx->pending = 1;
        }
    }

    return;
}


/**************************************************************
 * sendtofpga():  - Build and send a packet to the FPGA based
 *                  on which resource is being used
 **************************************************************/
static void sendtofpga(
    int     rscid,     // write to register for this resource
    TIFDEV *pctx,      // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the tif
    SLOT    *pslot;    // This peripheral's slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value
    int      pktlen;   // size of outgoing packet
    int      i,j;      // loop counters

    pslot = pctx->pslot;
    pmycore = pslot->pcore;
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
    pkt.core = (pslot->pcore)->core_id;

    if (rscid == RSC_LEDS) {
        pkt.reg = TIF_R_LED;   // LED
        pkt.count = 1;
        pkt.data[0] = pctx->leds;
        pktlen = 5;    // 4 header + 1 data
    }
    else if (rscid == RSC_TONE) {
        pkt.reg = TIF_R_TONE;   // Tone generator register
        pkt.count = 1;
        // duration is in milliseconds but register wants it in
        // units of 10 ms with a maximum of 31 units.  Limit the
        // duration, scale it, and put it in the low bits.
        pkt.data[0] = (pctx->dur > 310) ? 31 : (pctx->dur / 10);
        // UI has tone in range 1 to 4 but register wants it zero indexed.
        pkt.data[0] |= (pctx->tone - 1) << 5;
        pkt.data[0] |= pctx->vol << 7;
        pktlen = 5;    // 4 header + 1 data
    }
    else if (rscid == RSC_TEXT) {
        pkt.reg = TIF_R_FIFO;      // FPGA fifo for text/cmds
        // Send up to 12 characters to the FPGA.  Recall that characters
        // are 9 bits.  Bit 8 is set for text and cleared for commands.
        // Copy from our local FIFO and break into two bytes.  The low
        // nibble in the first byte and high 5 bits in the next.  We
        // always start from fifo[0] since we know it has not been sent.
        j = pctx->idx;            // num 9-bit chars to put in pkt
        j = (j <= 15) ? j : 15;   // at most 12 to match FIFO in FPGA 
        for (i = 0; i < (2 * j); i++ ) {
            pkt.data[i] = pctx->fifo[i / 2] & 0x0f;
            // set bit 7 to push the character onto the FIFO
            i++;
            pkt.data[i] = ((pctx->fifo[i / 2] >> 4) & 0x1f) | 0x80 ;
        }
        pkt.count = 2 * j;
        pktlen = 4 + pkt.count;  // 4 header plus data bytes
    }
    // Packet is built.  Send it and start an ACK timer if needed.
    txret = dpi_tx_pkt(pmycore, &pkt, pktlen);
    if (txret != 0) {
        // the send of the new pin values did not succeed.  This
        // probably means the input buffer to the USB port is full.
        // Tell the user of the problem.
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    if (pctx->ptimer == 0) {
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);
    }

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void   *timer,   // handle of the timer that expired
    TIFDEV *pctx)    // the peripheral with a timeout
{
    // Log the missing ack
    edlog(E_NOACK);

    // We want to clear the packet pending flag for text or commands.
    // This will also clear it for missing ack for tone and leds too.
    pctx->pending = 0;

    return;
}

// end of tif.c
