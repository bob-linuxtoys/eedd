/*
 * Name: lcd6.c
 *
 * Description: Driver for the lcd6 display card
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

/* 
 * display : Six 7-segment digits
 *    Characters written to this node are displayed.  The characters 
 * must be taken from the above set and only the first six characters
 * of the input line are displayed.  The exception to this are decimal
 * points which are displayed between the characters and which do not
 * count toward the six character limit.  Messages with less than six
 * characters are left justified.  Some examples if the display is in
 * slot 2:
 * 	# display 123456
 *         telnet localhost 8880
 * 	dpset 6 display 123456
 * 	# display 8.8.8.8.8
 * 	dpset 6 display 8.8.8.8.8
 * 	# Display 12 left justified
 * 	dpset 6 display 12
 * 
 * segments : Individual segment control
 *    You can directly control which segments are displayed by writing
 * six space-separated hexadecimal values to the segments resource.
 * The MSB of each value controls the 'a' segment and the next-MSB value
 * controls the 'b' segment.  The LSB controls the decimal point.  For
 * example:
 * 	# display the middle bar (segment g)  on the first three
 * 	# digits and the leftmost two vertical bars (segments 'e'
 * 	# and 'f') on the last three digits
 * 	dpset 6 segments 80 80 80 60 60 60
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
#define MAX_LINE_LEN  100       // max line length from the user
#define MAX_TEXT_LEN  12        // "8.8.8.8.8\0"
#define LCD_PKT_LEN   52
#define LCD_REG_BASE  0
#define NDIGITS       6
        // resource names and numbers
#define RSC_DISP     1
#define RSC_SEGS     2
#define RSC_CONFIG   3
        // Log messages
#define M_LCD6MEM  "unable to allocate memory for lcd6 driver."
#define M_LCD6DPKT "bogus write response packet"
#define M_LCD6USER "bogus data from user"
        // Segment to bit position mapping
#define SP           0
#define SA           1
#define SB           2
#define SC           3
#define SD           4
#define SE           5
#define SF           6
#define SG           7
        // LCD position on board (or legacy) as an index
#define LCDTOP    0
#define LCDLEG    1
#define NUMLCDPOS 2



/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an lcd6
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    void    *ptimer;   // timer to watch for dropped ACK packets
    // device specific information
    char     text[MAX_TEXT_LEN];  // Text to display
    int      segs[NDIGITS]; // The segments
    int      type;     // top, bot, legacy
} LCD6DEV;

    // character to 7-segment mapping
typedef struct
{
    char sym;               // character to map
    int  segval;            // 7 segment equivalent
} SYMBOL;

SYMBOL symbols[] = {   // segments MSB -> gfedcbap <- LSB
    {'0', 0x7e }, {'1', 0x0c }, {'2', 0xb6 }, {'3', 0x9e },
    {'4', 0xcc }, {'5', 0xda }, {'6', 0xfa }, {'7', 0x0e },
    {'8', 0xfe }, {'9', 0xce }, {'a', 0xee }, {'b', 0xf8 },
    {'c', 0x72 }, {'d', 0xbc }, {'e', 0xf2 }, {'f', 0xe2 },
    {'A', 0xee }, {'B', 0xf8 }, {'C', 0x72 }, {'D', 0xbc },
    {'E', 0xf2 }, {'F', 0xe2 }, {'o', 0xb8 }, {'L', 0x70 },
    {'r', 0xa0 }, {'h', 0xe8 }, {'H', 0xec }, {'-', 0x80 },
    {' ', 0x00 }, {'_', 0x10 }, {'u', 0x38 }, {'.', 0x01 }
};
#define NSYM (sizeof(symbols) / sizeof(SYMBOL))

    // location of a segment in terms of low, mid, or high byte
    // and which bit in that byte to set.  
typedef struct
{
    unsigned char     lmh;            // low,mid,high as 1, 2, or 4 
    int               bitpos;         // which bit of the 74595 to set
} SEGLOC ;

    // There are 48 segments and 3 types of LCD connections ordered
    // as Top, Bottom, and Legacy
SEGLOC  segdrv[54][2] = {
    /* 1p  */ {{4,1},{4,5}},
    /* 1a  */ {{4,2},{4,3}},  // {lmh,bitpos}
    /* 1b  */ {{4,1},{4,5}},
    /* 1c  */ {{4,1},{4,5}},
    /* 1d  */ {{4,2},{4,3}},
    /* 1e  */ {{4,3},{4,4}},
    /* 1f  */ {{4,3},{4,4}},
    /* 1g  */ {{4,2},{4,3}},
    /* 2p  */ {{2,6},{4,1}},
    /* 2a  */ {{2,5},{4,0}},
    /* 2b  */ {{2,6},{4,1}},
    /* 2c  */ {{2,6},{4,1}},
    /* 2d  */ {{2,5},{4,0}},
    /* 2e  */ {{4,0},{4,2}},
    /* 2f  */ {{4,0},{4,2}},
    /* 2g  */ {{2,5},{4,0}},
    /* 3p  */ {{2,7},{2,7}},
    /* 3a  */ {{1,5},{2,5}},
    /* 3b  */ {{2,7},{2,7}},
    /* 3c  */ {{2,7},{2,7}},
    /* 3d  */ {{1,5},{2,5}},
    /* 3e  */ {{1,7},{2,6}},
    /* 3f  */ {{1,7},{2,6}},
    /* 3g  */ {{1,5},{2,5}},
    /* 4p  */ {{2,4},{2,2}},
    /* 4a  */ {{1,4},{2,3}},
    /* 4b  */ {{2,4},{2,2}},
    /* 4c  */ {{2,4},{2,2}},
    /* 4d  */ {{1,4},{2,3}},
    /* 4e  */ {{1,6},{2,4}},
    /* 4f  */ {{1,6},{2,4}},
    /* 4g  */ {{1,4},{2,3}},
    /* 5p  */ {{2,3},{1,7}},
    /* 5a  */ {{1,2},{2,0}},
    /* 5b  */ {{2,3},{1,7}},
    /* 5c  */ {{2,3},{1,7}},
    /* 5d  */ {{1,2},{2,0}},
    /* 5e  */ {{1,3},{2,1}},
    /* 5f  */ {{1,3},{2,1}},
    /* 5g  */ {{1,2},{2,0}},
    /* 6p  */ {{0,7},{0,0}},  // there is no decimal point on the 6th digit
    /* 6a  */ {{1,0},{1,4}},
    /* 6b  */ {{2,2},{1,5}},
    /* 6c  */ {{2,2},{1,5}},
    /* 6d  */ {{1,0},{1,4}},
    /* 6e  */ {{1,1},{1,6}},
    /* 6f  */ {{1,1},{1,6}},
    /* 6g  */ {{1,0},{1,4}},
    /* C1+ */ {{4,6},{4,6}},  // locations of the common lines
    /* C1- */ {{4,5},{1,0}},
    /* C2+ */ {{2,1},{1,2}},
    /* C2- */ {{2,0},{1,3}},
    /* C5+ */ {{4,7},{4,7}},
    /* C5- */ {{4,4},{1,1}},
};
#define COM1P 48
#define COM1N 49
#define COM2P 50
#define COM2N 51
#define COM5P 52
#define COM5N 53

/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void setsegdrv(int, int, int, unsigned char *, LCD6DEV *);
static void lcd6user(int, int, char*, SLOT*, int, int*, char*);
static void text_to_segs(char *, int *);
static int  lcd6tofpga(LCD6DEV *);
static void noAck(void *, LCD6DEV *);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    LCD6DEV   *pctx;   // our local device context

    // Allocate memory for this peripheral
    pctx = (LCD6DEV *) malloc(sizeof(LCD6DEV));
    if (pctx == (LCD6DEV *) 0) {
        // Malloc failure this early?
        edlog(M_LCD6MEM);
        return (-1);
    }

    // Init our LCD6DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response
    pctx->type = LCDTOP;       // legacy LCD

    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_DISP].name = "display";
    pslot->rsc[RSC_DISP].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_DISP].bkey = 0;
    pslot->rsc[RSC_DISP].pgscb = lcd6user;
    pslot->rsc[RSC_DISP].uilock = -1;
    pslot->rsc[RSC_DISP].slot = pslot;
    pslot->rsc[RSC_SEGS].name = "segments";
    pslot->rsc[RSC_SEGS].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_SEGS].bkey = 0;
    pslot->rsc[RSC_SEGS].pgscb = lcd6user;
    pslot->rsc[RSC_SEGS].uilock = -1;
    pslot->rsc[RSC_SEGS].slot = pslot;
    pslot->name = "lcd6";
    pslot->desc = "Six digit 7-segment LCD display";
    pslot->help = README;

    strcpy(pctx->text, "      ");
    text_to_segs(pctx->text, pctx->segs);
    (void) lcd6tofpga(pctx);     // Send segments to the FPGA

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
    LCD6DEV *pctx;    // our local info

    pctx = (LCD6DEV *)(pslot->priv);  // Our "private" data is a LCD6DEV

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  //Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // There are no other packets from the LCD6 FPGA code so if we
    // get here there is a problem.  Log the error.
    edlog("invalid lcd6 packet from board to host");

    return;
}


/**************************************************************
 * lcd6user():  - The user is reading or writing to the display
 * Get the value and update the lcd6 on the BaseBoard or copy the
 * current value into the supplied buffer.
 **************************************************************/
static void lcd6user(
    int      cmd,      //==DPGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    LCD6DEV *pctx;     // our local info
    int      ret;      // return count
    int      txret;    // ==0 if the packet went out OK
    int      ssret;    // string scan return value
    int      ns[NDIGITS];  // new segment values
    int      i;        // temp counter


    pctx = (LCD6DEV *) pslot->priv;


    // Look for input from the user, the most common case
    // Is this a display update?
    if ((cmd == EDSET) && (rscid == RSC_DISP )) {
        strncpy(pctx->text, val, MAX_TEXT_LEN - 1);
        pctx->text[MAX_TEXT_LEN - 1] = (char) 0;  // as a precaution
        text_to_segs(pctx->text, pctx->segs);
    }
    // Is this a direct write to the segments?
    else if ((cmd == EDSET) && (rscid == RSC_SEGS )) {
        ssret = sscanf(val, "%x %x %x %x %x %x", &(ns[0]), &(ns[1]),
                       &(ns[2]), &(ns[3]), &(ns[4]), &(ns[5]));
        if (ssret != 6) {
            // quietly ignore bogus input from user
            return;
        }
        for (i = 0; i < NDIGITS; i++) {
            pctx->segs[i] = ns[i] & 0x00ff;
        }
    }

    // Return the display value or the segment values
    else if ((cmd == EDGET) && (rscid == RSC_DISP )) {
        ret = snprintf(buf, *plen, "%s\n", pctx->text);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDGET) && (rscid == RSC_SEGS )) {
        ret = snprintf(buf, *plen, "%02x %02x %02x %02x %02x %02x\n",
                 pctx->segs[0], pctx->segs[1], pctx->segs[2], pctx->segs[3],
                 pctx->segs[4], pctx->segs[5]);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // To get here means we had a valid update of the display or segments
    txret =  lcd6tofpga(pctx);   // This peripheral's context
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
 * setsegdrv():  - Given the state of the common lines, set the
 * segment lines to display the pattern set by the user.
 **************************************************************/
static void setsegdrv(
          int c1,          // COM value: -1 for ground, 0 for .5 Vcc,
          int c2,          // and 1 for Vcc
          int c5,
          unsigned char *pd,   // pointer to packet data
          LCD6DEV * pctx)
{
    // HOW THIS WORKS
    // The LCD multiplexes the segments with three common lines
    // and eighteen segment drive lines.  Here is the wiring of
    // LCD6 card to the LCD:
    // 
    //   ________CARD_______
    //   Top  Bottom  Legacy    LCD     COM1    COM25   COM50
    //   hd     hx     ha7      49                      COM50+
    //   hd     hx     ha7      50                      COM50+
    //   hb     hx     hb6      1       COM1+
    //   hb     hx     hb6      2       COM1+
    //   hg     hx     hc5      47      1c      1dp     1b
    //   he     hx     hd4      3       1e              1f
    //   hf     hx     he3      4       1g      1d      1a
    //   hh     hx     hf2      6       2e              2f
    //   mb     hx     hg1      43      2c      2dp     2b
    //   mc     hx     hh0      7       2g      2d      2a
    //   ma     hx     ma7      39      3c      3dp     3b
    //   la     hx     mb6      9       3e              3f
    //   lc     hx     mc5      10      3g      3d      3a
    //   lb     hx     md4      13      4e              4f
    //   ld     hx     me3      14      4g      4d      4a
    //   md     hx     mf2      35      4c      4dp     4b
    //   le     hx     mg1      17      5e              5f
    //   lf     hx     mh0      18      5g      5d      5a
    //   me     hx     la7      31      5c      5p      5b
    //   lg     hx     lb6      21      6e              6f
    //   mf     hx     lc5      28      6c              6b
    //   lh     hx     ld4      22      6g      6d      6a
    //   mh     hx     le3      26              COM25+
    //   mh     hx     le3      27              COM25+
    //   mg     hx     lf2      24              COM25-
    //   mg     hx     lf2      25              COM25-
    //   ha     hx     lg1      COM50-
    //   ha     hx     lh0      COM1-
    // 
    // 
    //
    // C1 refers to the first common line, COM1.  C2 is COM25 and
    // C5 is COM50.  The 'h', 'm', and 'l' on the first line refer
    // to the out25's high, mid, and low bytes respectively.  It is
    // a little confusing that both the 74595 and 7-segment displays
    // use the letters a-g to designate the outputs or segments.
    // If prefaced with h, m, or l (above) it is for the 74595.  If
    // prefaced with 1 to 6, it is a segment.

    // An LCD segment is lit if the segment drive voltage is opposite
    // that of the common.  So set the common line low and set the
    // segment line high.  Segments respond to changes so you have to
    // swap the values on the common and segment at a couple hundred
    // Hertz.  The problem is that a segment has three common lines
    // and setting the segment value for one common line might light
    // up the segments on the other common lines.  To get around this
    // problem we set the unused COM lines to 0.5 Vcc and do a quick
    // switch from 0 to 1 on the segment.  The average voltage on the
    // unused segments is thus 0.5 Vcc and so nothing is displayed.
 
    // The LCD card takes three bits at a time from the FPGA. The lcd6
    // uses a 48x3 RAM for input from the host.  Each byte in the RAM
    // sets three bits on the LCD6 card.  The pattern is:
    //        Byte bit2 bit1 bit0
    //         0     hh   mh   lh
    //         1     hg   mg   lg
    //         2     hf   mf   lf
    //         3     he   me   le
    //         4     hd   md   ld
    //         5     hc   mc   lc
    //         6     hb   mb   lb
    //         7     ha   ma   la
    // 

    int  *segs;        // segment values array
    int   i, j;
    int   mask, bitpos;
    unsigned char  lmh;
    int   segval;      // == 1 if segment is on

    segs = pctx->segs;

    for (i = 0; i < 8; i++)
        pd[i] = 0;

    // for each digit, for each segment
    for (i = 0; i < 6; i++) {
        mask = 0x01;
        for (j = 0; j < 8; j++) {
            if ((j == 0) && (i == 5)) {   // no dp on digit 6
            mask = mask << 1;   // shift mask to next segment in digit
                continue;
            }
            // get LCD connection for segment i,j.  
            lmh = segdrv[(i*8)+j][pctx->type].lmh;
            bitpos = segdrv[(i*8)+j][pctx->type].bitpos;
            // get segment value
            segval = ((segs[i] & mask) == mask) ? 1 : 0;
            // j=76543210 <=> seg=GFEDCBAdp
            // segments C, E, and G are on COM1
            if ((j == 3) || (j == 5) || (j == 7)) {
                // set lcd segment drive based on segment's common voltage
                if (((c1 == -1) && (segval == 1))
                    || ((c1 == 1) && (segval == 0))) {
                    pd[bitpos] |= lmh;
                }
            }
            // segments D and DP are on COM25
            else if ((j == 4) || (j == 0)) {
                if (((c2 == -1) && (segval == 1))
                    || ((c2 == 1) && (segval == 0))) {
                    pd[bitpos] |= lmh;
                }
            }
            // segments A, B, and F are on COM50
            else if ((j == 1) || (j == 2) || (j == 6)) {
                if (((c5 == -1) && (segval == 1))
                    || ((c5 == 1) && (segval == 0))) {
                    pd[bitpos] |= lmh;
                }
            }
            mask = mask << 1;   // shift mask to next segment in digit
        }
    }
    // set the COMMON voltages
    if (c1 == 1) {
        pd[segdrv[COM1P][pctx->type].bitpos] |= segdrv[COM1P][pctx->type].lmh;  // COM1 is Vcc
        pd[segdrv[COM1N][pctx->type].bitpos] |= segdrv[COM1N][pctx->type].lmh;
    }
    else if (c1 == -1) {
        pd[segdrv[COM1P][pctx->type].bitpos] |= 0;  // COM1 is 0
        pd[segdrv[COM1N][pctx->type].bitpos] |= 0;
    }
    else if (c1 == 0) {
        pd[segdrv[COM1P][pctx->type].bitpos] |= segdrv[COM1P][pctx->type].lmh;  // COM1 is Vcc/2
        pd[segdrv[COM1N][pctx->type].bitpos] |= 0;
    }
    if (c2 == 1) {
        pd[segdrv[COM2P][pctx->type].bitpos] |= segdrv[COM2P][pctx->type].lmh;  // COM2 is Vcc
        pd[segdrv[COM2N][pctx->type].bitpos] |= segdrv[COM2N][pctx->type].lmh;
    }
    else if (c2 == -1) {
        pd[segdrv[COM2P][pctx->type].bitpos] |= 0;  // COM2 is 0
        pd[segdrv[COM2N][pctx->type].bitpos] |= 0;
    }
    else if (c2 == 0) {
        pd[segdrv[COM2P][pctx->type].bitpos] |= segdrv[COM2P][pctx->type].lmh;  // COM2 is Vcc/2
        pd[segdrv[COM2N][pctx->type].bitpos] |= 0;
    }
    if (c5 == 1) {
        pd[segdrv[COM5P][pctx->type].bitpos] |= segdrv[COM5P][pctx->type].lmh;  // COM5 is Vcc
        pd[segdrv[COM5N][pctx->type].bitpos] |= segdrv[COM5N][pctx->type].lmh;
    }
    else if (c5 == -1) {
        pd[segdrv[COM5P][pctx->type].bitpos] |= 0;  // COM5 is 0
        pd[segdrv[COM5N][pctx->type].bitpos] |= 0;
    }
    else if (c5 == 0) {
        pd[segdrv[COM5P][pctx->type].bitpos] |= segdrv[COM5P][pctx->type].lmh;  // COM5 is Vcc/2
        pd[segdrv[COM5N][pctx->type].bitpos] |= 0;
    }
    return;
}


/**************************************************************
 * text_to_segs():  - Convert the given text to its 7-segment
 * equivalent.
 **************************************************************/
static void text_to_segs(char *text, int *segs)
{
    int   i;           // index into segs[]
    int   j;           // index into symbols[]
    int   k;           // index into text

    k = 0;
    for (i = 0; i < NDIGITS; i++) {
        segs[i] = 0;

        for (j = 0; j < NSYM; j++) {
            if (text[k] == symbols[j].sym) {
                segs[i] = symbols[j].segval;
                break;
            }
        }

        if ((text[k] != '.') && (text[k+1] == '.')) {
            segs[i] |= 1;     // decimal point is LSB of segments
            k++;
        }
        k++;
    }
}


/**************************************************************
 * lcd6tofpga():  - Send timing/bit pattern to the FPGA card.
 * Return zero on success
 **************************************************************/
static int lcd6tofpga(
    LCD6DEV *pctx)     // This peripheral's context
{
    DP_PKT   pkt;      // send write and read cmds to the lcd6
    CORE    *pmycore;  // FPGA peripheral info
    SLOT    *pmyslot;  // This peripheral's slot info
    int      txret;

    pmyslot = pctx->pslot;
    pmycore = pmyslot->pcore;

    // See the protocol manual for a description of the registers.
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = (pmyslot->pcore)->core_id;
    pkt.reg = LCD_REG_BASE;
    pkt.count = LCD_PKT_LEN - 4;
    // Compute and set segments drive values for each combination of COMx
    setsegdrv(1, 0, 0, &(pkt.data[0]), pctx);    // COMs = Vcc, 0.5 Vcc, 0.5 Vcc
    setsegdrv(0, 1, 0, &(pkt.data[8]), pctx);    // COMs = 0.5 Vcc, Vcc, 0.5 Vcc
    setsegdrv(0, 0, 1, &(pkt.data[16]), pctx);   // COMs = 0.5 Vcc, 0.5 Vcc, Vcc
    setsegdrv(-1, 0, 0, &(pkt.data[24]), pctx);  // COMs = 0, 0.5 Vcc, 0.5 Vcc
    setsegdrv(0, -1, 0, &(pkt.data[32]), pctx);  // COMs = 0.5 Vcc, 0, 0.5 Vcc
    setsegdrv(0, 0, -1, &(pkt.data[40]), pctx);  // COMs = 0.5 Vcc, 0.5 Vcc, 0

    txret = dpi_tx_pkt(pmycore, &pkt, LCD_PKT_LEN);

    return(txret);
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void    *timer,   // handle of the timer that expired
    LCD6DEV *pctx)    // points to instance of this peripheral
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}
// end of lcd6.c
