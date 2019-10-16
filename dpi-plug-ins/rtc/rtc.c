/*
 *  Name: rtc.c
 *
 *  Description: Driver for the rtc peripheral
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
 *    value     - read/write resource to set pot values as a percentage of FS
 *    time      - the current time in the RTC chip
 *                 FORMAT: Sat Sep 10 14:42:00 PDT 2018
 *    alarm     - time at which the alarm output on the RTC card will go low.
 *    state     - state of the alarm output.  One of 'off', 'enabled', 'alarm',
 *                or 'triggered'
 *
 * Copyright:   Copyright (C) 2017-2019 Demand Peripherals, Inc.
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
#include <time.h>
#include <syslog.h>
#include <errno.h>
#include <ctype.h>
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
#define MXLINELN            100
#define FN_TIME             "time"
#define FN_ALARM            "alarm"
#define FN_STATE            "state"
        // Resource index numbers
#define RSC_TIME            0
#define RSC_ALARM           1
#define RSC_STATE           2
        // PCF2123 commands and defines
#define PCF_CMD_READ        0x90   // start reg in low 4 bits
#define PCF_CMD_WRITE       0x10   // start reg in low 4 bits

// rtc local context
typedef struct
{
    SLOT    *pSlot;         // handle to peripheral's slot info
    int      flowCtrl;      // ==1 if we are applying flow control
    int      xferpending;   // ==1 if we are waiting for a reply
    int      getrsc;        // which resource user is reading
    int      nbxfer;        // number of bytes sent in transfer
    void    *ptimer;        // Watchdog timer to abort a failed transfer
    struct tm uitm;         // time taken from a UI string
} RTCDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void  packet_hdlr(SLOT *, DP_PKT *, int);
static void  user_hdlr(int, int, char*, SLOT*, int, int*, char*);
static void  noAck(void *, RTCDEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);
// The following should be in time.h
char        *strptime(const char *s, const char *format, struct tm *tm);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    RTCDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (RTCDEV *) malloc(sizeof(RTCDEV));
    if (pctx == (RTCDEV *) 0) {
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
    pslot->rsc[RSC_TIME].name = FN_TIME;
    pslot->rsc[RSC_TIME].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_TIME].bkey = 0;
    pslot->rsc[RSC_TIME].pgscb = user_hdlr;
    pslot->rsc[RSC_TIME].uilock = -1;
    pslot->rsc[RSC_TIME].slot = pslot;
    pslot->rsc[RSC_ALARM].name = FN_ALARM;
    pslot->rsc[RSC_ALARM].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_ALARM].bkey = 0;
    pslot->rsc[RSC_ALARM].pgscb = user_hdlr;
    pslot->rsc[RSC_ALARM].uilock = -1;
    pslot->rsc[RSC_ALARM].slot = pslot;
    pslot->rsc[RSC_STATE].name = FN_STATE;
    pslot->rsc[RSC_STATE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_STATE].bkey = 0;
    pslot->rsc[RSC_STATE].pgscb = user_hdlr;
    pslot->rsc[RSC_STATE].uilock = -1;
    pslot->rsc[RSC_STATE].slot = pslot;
    pslot->name = "rtc";
    pslot->desc = "real time clock";
    pslot->help = README;

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
    RTCDEV *pctx;     // our local info
    RSC    *prsc;     // pointer one of this slot's resources
    int     sec,min,hour,day,mon,year;  // time from the RTC
    int     amin,ahour,aday;       // alarm time from the RTC
    char    ob[MXLINELN];  // output buffer
    int     ob_len;   // length of line in ob

    pctx = (RTCDEV *)(pslot->priv);  // Our "private" data is a RTCDEV
    ob[MXLINELN-1] = (char) 0;


    // Packets are either a write reply or an auto send SPI reply.
    // The auto-send packet should have a count two (for the 2 config bytes)
    // and the number of bytes in the SPI packet (nbxfer).
    if (!(( //autosend packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 16))
          ||    ( // write response packet for mosi data packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_COUNT) && (pkt->count == (1 + pctx->nbxfer)))
          ||     ( // write response packet for config
           (((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 1))) ) ) {
        // unknown packet
        edlog("invalid rtc packet from board to host");
        return;
    }

    // Return if just the write reply
    if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA)
        return;

    // At this point we have a read response packet.  The packet
    // has registers 01 through 0B.  Extract and format the data
    // depending on which resource was requested.
    // Data in the chip is in BCD format.  We have to convert to binary
    if (pctx->getrsc == RSC_TIME) {
        sec   = ((pkt->data[0x04] >> 4) & 0x07) * 10;
        sec  += pkt->data[0x04] & 0x0f;
        min   = ((pkt->data[0x05] >> 4) & 0x07) * 10;
        min  += pkt->data[0x05] & 0x0f;
        hour  = ((pkt->data[0x06] >> 4) & 0x07) * 10;
        hour += pkt->data[0x06] & 0x0f;
        day   = ((pkt->data[0x07] >> 4) & 0x03) * 10;
        day  += pkt->data[0x07] & 0x0f;
        mon   = ((pkt->data[0x09] >> 4) & 0x01) * 10;
        mon  += pkt->data[0x09] & 0x0f;
        year  = ((pkt->data[0x0a] >> 4) & 0x0f) * 10;
        year += (pkt->data[0x0a] & 0x0f) + 2000;
        ob_len = snprintf(ob, MXLINELN-1, "%4d-%02d-%02d %02d:%02d:%02d\n",
                     year, mon, day, hour, min, sec);
    }
    else if (pctx->getrsc == RSC_ALARM) {
        amin   = ((pkt->data[0x0b] >> 4) & 0x07) * 10;
        amin  += pkt->data[0x0b] & 0x0f;
        ahour  = ((pkt->data[0x0c] >> 4) & 0x07) * 10;
        ahour += pkt->data[0x0c] & 0x0f;
        aday   = ((pkt->data[0x0d] >> 4) & 0x03) * 10;
        aday  += pkt->data[0x0d] & 0x0f;
        ob_len = snprintf(ob, MXLINELN-1, "%02d %02d:%02d\n",
                     aday, ahour, amin);
    }
    else if (pctx->getrsc == RSC_STATE) {
        if (pkt->data[3] == 0x00)
            ob_len = sprintf(ob, "off\n");
        else if (pkt->data[3] == 0x02)
            ob_len = sprintf(ob, "enabled\n");
        else if (pkt->data[3] == 0x0a)
            ob_len = sprintf(ob, "alarm\n");
        else if ((pkt->data[3] & 0x40) == 0x40)
            ob_len = sprintf(ob, "on\n");
        else
            ob_len = sprintf(ob, "unknown\n");
    }
    else {
        return;  // should not get here
    }

    prsc = &(pslot->rsc[pctx->getrsc]);
    send_ui(ob, ob_len, prsc->uilock);
    prompt(prsc->uilock);

    // Response sent so clear the lock and response timer
    prsc->uilock = -1;
    del_timer(pctx->ptimer);  //Got the response
    pctx->ptimer = 0;

    return;
}


/**************************************************************
 * user_hdlr():  - The user is reading or writing resources.
 * The three resources are wall clock time, alarm time, and
 * the state of the alarm.  The driver keeps no internal time
 * or state information so every resource read uses an SPI
 * packet,
 *  The registers in the PCF2123 are:
 *     00h Control_1 EXT_TEST 0 STOP SR 0 12_24 CIE 0
 *     01h Control_2 MI SI MSF TI_TP AF TF AIE TIE
 *     02h Seconds (0 to 59)
 *     03h Minutes (0 to 59)
 *     04h Hours (0 to 23) in 24 h mode
 *     05h Days (1 to 31)
 *     06h Weekdays (0 to 6)
 *     07h Months (1 to 12)
 *     08h Years (0 to 99)
 *     09h Minute_alarm (0 to 59)
 *     0Ah Hour_alarm (0 to 23) in 24 h mode
 *     0Bh Day_alarm (1 to 31)
 *     0Ch Weekday_alarm  (not used)
 *     0Dh Offset_register (not used)
 *     0Fh Countdown_timer (not used)
 *
 * Setting time affects registers 02 to 08.
 * Setting the alarm affects registers 01 and 09 to 0B.
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
    RTCDEV  *pctx;     // our local info
    DP_PKT   pkt;      // packet to the FPGA card
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // count of successful sscanf 
    int      sec,min,hour,day,mon,year;  // time to the RTC
    int      amin,ahour,aday;       // alarm time to the RTC
    char     newstate[MXLINELN];

    pctx = (RTCDEV *) pslot->priv;
    pmycore = pslot->pcore;

    if (cmd == EDGET) {
        // Reading any resource causes a read from the device.
        // The packet handler sorts out what to return to the user.
        // Read registers 01 through 0B (11 regs) + 1 for the count
        pkt.data[1] = PCF_CMD_READ | 0x01;    // read from reg 01
        pkt.data[0] = 13;                     // 11 + cmd + count

        // tell the pkt handler which resource is being read,
        // lock the ui, and recort the number of bytes sent
        pctx->getrsc = rscid;
        pslot->rsc[rscid].uilock = (char) cn;
        pctx->nbxfer = pkt.data[0];
    }
    else if ((cmd == EDSET) && (rscid == RSC_TIME)) {
        // 2018-09-21 14:45:23
        ret = sscanf(val, "%4d-%2d-%2d %2d:%2d:%2d",
                     &year, &mon, &day, &hour, &min, &sec);
        if ((ret != 6) || (sec > 59) || (sec < 0) || (min > 59) ||
            (min < 0) || (hour > 23) || (hour < 0) || (day > 31) ||
            (day < 1) || (mon > 12) || (mon < 1)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // write date/time into regs 02 to 08 + write_cmd + count
        pkt.data[0] = 9;                      // count+cmd+regs
        pkt.data[1] = PCF_CMD_WRITE | 0x02;   // write from reg 02
        pkt.data[2] = (sec % 10) + ((sec / 10) << 4);
        pkt.data[3] = (min % 10) + ((min / 10) << 4);
        pkt.data[4] = (hour % 10) + ((hour / 10) << 4);
        pkt.data[5] = (day % 10) + ((day / 10) << 4);
        pkt.data[6] = 0;  // dummy value for weekday
        pkt.data[7] = (mon % 10) + ((mon / 10) << 4);
        pkt.data[8] = (year % 10) + (((year - 2000) / 10) << 4);
    }
    else if ((cmd == EDSET) && (rscid == RSC_ALARM)) {
        ret = sscanf(val, "%2d %2d:%2d\n", &aday, &ahour, &amin);
        if ((ret != 3) || (amin > 59) || (amin < 0) || (ahour > 23) ||
            (ahour < 0) || (aday > 31) || (aday < 1)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // write day/hour/min for alarm, regs 09 to 0c
        pkt.data[0] = 6;                      // count+cmd+regs
        pkt.data[1] = PCF_CMD_WRITE | 0x09;   // write from reg 09
        pkt.data[2] = (amin % 10) + ((amin / 10) << 4);
        pkt.data[3] = (ahour % 10) + ((ahour / 10) << 4);
        pkt.data[4] = (aday % 10) + ((aday / 10) << 4);
        pkt.data[5] = 0x80;   // disable weekday alarm
    }
    else if ((cmd == EDSET) && (rscid == RSC_STATE)) {
        // looking for off, on, or enabled
        newstate[MXLINELN-1] = (char) 0;
        ret = sscanf(val, "%s", newstate);
        newstate[0] = tolower(newstate[0]);
        newstate[1] = tolower(newstate[1]);
        if (!((ret == 1) &&
              ((newstate[0] == 'e') ||
               ((newstate[0] == 'o') &&
                ((newstate[1] == 'n') || (newstate[1] == 'f'))))))
        {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        // New state.  Send new state to card
        pkt.data[0] = 3;                      // count+cmd+regs
        pkt.data[1] = PCF_CMD_WRITE | 0x01;   // write from reg 01
        pkt.data[2] = (newstate[0] == 'e') ? 0x02 :  // int on alarm
                      (newstate[1] == 'n') ? 0x40 :  // int on second change
                      (newstate[1] == 'f') ? 0x00 :  // all off
                                             0x00;   // default
    }

    // to get here means we correctly parsed a UI command and
    // need to send out the SPI packet
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = (pslot->pcore)->core_id;
    pkt.reg = QCSPI_REG_COUNT;
    pkt.count = pkt.data[0];       // sending count plus all SPI pkt bytes
    // try to send the packet.  Schedule a resend on tx failure

    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data
    if (txret != 0) {
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);

    // Nothing to send back to the user
    *plen = 0;

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void     *timer,   // handle of the timer that expired
    RTCDEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

//end of dopt2.c
