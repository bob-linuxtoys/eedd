/*
 *  Name: enumerator.c
 *
 *  Description: Interface to a Demand Peripherals FPGA card
 *
 *  Resources:
 *    port      -  full path to serial port (/dev/ttyUSB0)
 *    license   -  License for the FPGA image
 *    copyright -  Copyright of the FPGA image
 */

/*
 * Copyright:   Copyright (C) 2019 by Demand Peripherals, Inc.
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
 */

/*
 *    The enumerator peripheral is a read-only memory in the FPGA
 *  image that has a list of useful information.  It has the image
 *  copyright, the email address of the person who accepted the 
 *  end user license agreement, and the date of the build.  The 
 *  important information is a list of the peripherals that are 
 *  in the build.
 *
 *    The enumerator has a single 8-bit register that is accessed
 *  as a FIFO.  Typically you would read the ROM by issuing a series
 *  of 8-bit reads of 255 words from register 0.  An internal counter
 *  keeps track of the reads so that subsequent reads pick up the
 *  next character to send.  You may reset the internal counter by
 *  doing any 8-bit write to register 0.
 *
 *    The information in the enumerator is broken into strings of
 *  text with the following meaning assigned to each string:
 *
 *    String 1:     Copyright
 *    String 2:     Licensee email address
 *    String 3:     Build date
 *    String 4:     (unused)
 *    String 5:     (unused)
 *    String 6:     (unused)
 *    String 7:     (unused)
 *    String 8:     (unused)
 *    String 9:     enumerator
 *    String 10:    bb4io
 *    String 11:    peripheral_core_2_name
 *    String 12:    peripheral_core_3_name
 *    String 13:    peripheral_core_4_name
 *    String 14:    peripheral_core_5_name
 *    String 15:    peripheral_core_6_name
 *    String 16:    peripheral_core_7_name
 *    String 17:    peripheral_core_8_name
 *    String 18:    peripheral_core_9_name
 *    String 19:    peripheral_core_10_name
 *
 *    The string 'peripheral_core_x_name' above is replaced by
 *  the name of the peripheral the user has selected for that
 *  core. Typical library names include "quadrature2", "dc2",
 *  and "servo8".  Note that the enumerator and BaseBoard I/O
 *  peripherals are always assigned to core zero and one
 *  respectively.
 *
 *  Registers:
 *    0: Enumeration text   - read only FIFO
 *    0: Address counter reset - write any value
 */


#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <limits.h>              // for PATH_MAX
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h> 
#include <linux/serial.h>
#include "eedd.h"
#include "readme.h"



/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // Reg # to read enumerator data from
#define ENUM_REG_TEXT        0x00
        // Reg # to write to in order to reset read address counter
#define ENUM_REG_RESET       0x00
        // Maximum number of bytes in the enumerator ROM
#define EROM_SZ              2048
typedef enum
{
    STATE_TIMER,       // Waiting for init delay timer to expire
    STATE_READING,     // Reading the ROM and awaiting responses
    STATE_DONE         // Done, become inactive
} ENUM_STATES;

        // SLIP decoder states
#define AWAITING_PKT  (1)
#define IN_PACKET     (2)
#define INESCAPE      (3)

        // resource names and numbers
#define FN_PORT            "port"
#define FN_LICENSE         "license"
#define FN_COPYRIGHT       "copyright"
#define RSC_PORT           0
#define RSC_LICENSE        1
#define RSC_COPYRIGHT      2
        // What we are is a ...
#define PLUGIN_NAME        "enumerator"
        // Default serial port
#define DEFDEV             "/dev/ttyUSB0"

#define MX_MSGLEN 1000


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an enumerator
typedef struct
{
    void    *pslot;            // handle to enumerator's slot info
    void    *ptimr;            // backup timer handle
    char     port[PATH_MAX];   // full path to serial port node
    int      usbFd;            // serial port File Descriptor (=-1 if closed)
    int      enumstate;        // where we are in the init process
    unsigned char slrx[RXBUF_SZ];  // raw received packet from USB port
    int      slix;             // where in slrx the next byte goes
    int      romIdx;           // current read location in rom
    char     rom[EROM_SZ];     // copy of ROM contents on the FPGA
    char    *license;          // license string
    char    *copyright;        // copyright string
    CORE     core[NUM_CORE];
} ENUM;



/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void receivePkt(int fd, void *priv, int rw);
static void process_pkt(SLOT *, DP_PKT *, int);
static int  getSoNames(ENUM *);
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static int  portsetup(ENUM *pctx);
static void InitStep2(void *, void *);
static int  dptoslip(unsigned char *, int, unsigned char *);
static void dispatch_packet(ENUM *pEnum, unsigned char *inbuf, int len);
static void initCore(CORE *pcore);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);
extern int  add_so(char *);
extern void initslot(SLOT *);
extern SLOT Slots[];
extern int  useStderr;
extern int  DebugMode;
extern int  ForegroundMode;
extern int  Verbosity;


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    ENUM    *pctx;     // our local port context
    int      i;        // generic loop counter
    int      ret;      // generic return value

    // Allocate memory for this plug-in
    pctx = (ENUM *) malloc(sizeof(ENUM));
    if (pctx == (ENUM *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in enumerator initialization");
        return (-1);
    }

    // Init our ENUM structure
    pctx->pslot = pslot;       // this instance of serial_fpga
    pctx->usbFd = -1;           // port is not yet open
    (void) strncpy(pctx->port, DEFDEV, PATH_MAX);

    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->priv = pctx;
    pslot->desc = "Demand Peripherals FPGA Interface";
    pslot->help = README;

    // Add handlers for the user visible resources
    pslot->rsc[RSC_PORT].slot = pslot;
    pslot->rsc[RSC_PORT].name = FN_PORT;
    pslot->rsc[RSC_PORT].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_PORT].bkey = 0;
    pslot->rsc[RSC_PORT].pgscb = usercmd;
    pslot->rsc[RSC_PORT].uilock = -1;
    pslot->rsc[RSC_LICENSE].name = FN_LICENSE;
    pslot->rsc[RSC_LICENSE].flags = IS_READABLE;
    pslot->rsc[RSC_LICENSE].bkey = 0;
    pslot->rsc[RSC_LICENSE].pgscb = usercmd;
    pslot->rsc[RSC_LICENSE].uilock = -1;
    pslot->rsc[RSC_LICENSE].slot = pslot;
    pslot->rsc[RSC_COPYRIGHT].name = FN_COPYRIGHT;
    pslot->rsc[RSC_COPYRIGHT].flags = IS_READABLE;
    pslot->rsc[RSC_COPYRIGHT].bkey = 0;
    pslot->rsc[RSC_COPYRIGHT].pgscb = usercmd;
    pslot->rsc[RSC_COPYRIGHT].uilock = -1;
    pslot->rsc[RSC_COPYRIGHT].slot = pslot;

    pctx->ptimr = (void *) 0;

    // init the Core structures
    for (i = 0; i < NUM_CORE; i++) {
        pctx->core[i].core_id   = i;
        pctx->core[i].soname    = (char *) 0;
        pctx->core[i].penum     = pctx;
        pctx->core[i].pcb       = (void *) 0;
    }

    // The enumerator can be in any eedd slot but is always
    // in core_id = 0.
    pctx->core[0].slot_id   = pslot->slot_id;
    pctx->core[0].soname    = "enumerator.so";
    pctx->core[0].pcb       = process_pkt;

    // now open and register the default port
    ret = portsetup(pctx);
    if (ret < 0) {
        return(0);           // unable to open the port
    }

    // add a timer to delay the completion of the init.  This
    // gives the user a chance to set the port
    pctx->ptimr = add_timer(ED_ONESHOT, 1000, InitStep2, (void *) pctx);

    return (0);
}


/**************************************************************
 * usercmd():  - The user is reading or setting a resource
 **************************************************************/
static void usercmd(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    ENUM    *pctx;     // serial_fpga private info
    int      ret;      // generic call return value.  Reused.

    // Get this instance of the plug-in
    pctx = (ENUM *) pslot->priv;


    if ((cmd == EDGET) && (rscid == RSC_PORT)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->port);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_LICENSE)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->license);
        *plen = ret;  // (errors are handled in calling routine)
    }
    if ((cmd == EDGET) && (rscid == RSC_COPYRIGHT)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->copyright);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDSET) && (rscid == RSC_PORT)) {
        // Val has the new port path.  Just copy it.
        (void) strncpy(pctx->port, val, PATH_MAX);
        // strncpy() does not force a null.  We add one now as a precaution
        pctx->port[PATH_MAX -1] = (char) 0;
        // close and unregister the old port
        if (pctx->usbFd >= 0) {
            del_fd(pctx->usbFd);
            close(pctx->usbFd);
            pctx->usbFd = -1;
        }
        // now open and register the new port
        ret = portsetup(pctx);
        if (ret < 0) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // add a timer kick off the reading of the ROM
        pctx->ptimr = add_timer(ED_ONESHOT, 10, InitStep2, (void *) pctx);
    }

    return;
}


/**************************************************************
 * Open the serial port.   Sets pctx->usbFd.
 * Return fd so errors can be handled in calling routine.
 **************************************************************/
static int portsetup(ENUM *pctx)
{
    struct serial_struct serial; // for low latency
    struct termios tbuf;

    if (pctx->usbFd < 0) {
        pctx->usbFd = open(pctx->port, (O_RDWR | O_NONBLOCK), 0);
        if (pctx->usbFd < 0) {
            return(pctx->usbFd);
        }
    }

    // port is open and can be configured
    tbuf.c_cflag = CS8 | CREAD | B115200 | CLOCAL;
    tbuf.c_iflag = IGNBRK;
    tbuf.c_oflag = 0;
    tbuf.c_lflag = 0;
    tbuf.c_cc[VMIN] = 1;        /* character-by-character input */
    tbuf.c_cc[VTIME] = 0;       /* no delay waiting for characters */
    int actions = TCSANOW;
    if (tcsetattr(pctx->usbFd, actions, &tbuf) < 0) {
        edlog(M_BADPORT, pctx->port, strerror(errno));
        exit(-1);
    }

    // Configure port for low latency
    ioctl(pctx->usbFd, TIOCGSERIAL, &serial); 
    serial.flags |= ASYNC_LOW_LATENCY;
    ioctl(pctx->usbFd, TIOCSSERIAL, &serial);

    // add callback for received characters
    add_fd(pctx->usbFd, ED_READ, receivePkt, (void *) pctx);
    return(pctx->usbFd);
}


/**************************************************************
 * process_pkt():  - Handle incoming packets from the FPGA board
 *   Ignore the write responses.  Read responses contain a section
 * of the ROM.  Store the read response data in the ROM section
 * of the ENUM struct.  Send another read request if more data is
 * required.
 *   
 **************************************************************/
static void process_pkt(
    SLOT     *pslot,   // handle for our slot's internal info
    DP_PKT   *pkt,     // the received packet
    int       len)     // number of bytes in the received packet
{
    ENUM    *pEnum;    // our local info
    DP_PKT   readpkt;  // the next read request packet
    int      stillneeded; // how many bytes are still needed
    int      i;        // generic loop counter

    pEnum = (ENUM *)(pslot->priv); // Our "private" data is an ENUM

    // Ignore write response packets
    if (pkt->cmd & DP_CMD_OP_WRITE)
        return;

    // Copy the data from the packet to our ROM storage area
    // (First data byte is at pkt[4] and last byte is a response count)
    // Do a sanity check
    if (pEnum->romIdx + len - 5 > EROM_SZ) {
        // Too many bytes in the ROM.  Something is messed up.
        // Return and let the timer expire and restart the ROM read
        return;
    }

    memcpy(&(pEnum->rom[pEnum->romIdx]), &(((char *) pkt)[4]), (len - 5));
    pEnum->romIdx += len - 5;

    // At this point the whole ROM image may be in place.  Test for
    // this and either send another read request or start initializing
    // the other peripherals
    if (pEnum->romIdx < EROM_SZ) {
        // Send out another read request.
        readpkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
        readpkt.core = 0;
        readpkt.reg = 0;
        stillneeded = EROM_SZ - pEnum->romIdx;
        readpkt.count = (stillneeded > 255) ? 255 : stillneeded;
        // Ignore write response packets
        (void) dpi_tx_pkt(&(pEnum->core[0]), &readpkt, 4);
        return;
    }

    // Parse ROM to put the driver names in the Core struct
    // On err, return to let the timer expire and try again
    if (getSoNames(pEnum) < 0)
        return;

    // The Core struct has all of the driver file names in place
    // Init the peripherals from 1 since the enumerator is done
    for (i = 1; i < NUM_CORE; i++) {
        initCore(&(pEnum->core[i]));
    }

    // Cancel the timer.
    del_timer(pEnum->ptimr);

    return;
}


/**************************************************************
 * InitStep2():  - Start getting the list of peripherals.
 *   The enumerator is a ROM in the FPGA image and has a list of
 * peripheral names.  Reading the enumerator takes several
 * reads and a state machine is used to step through the various
 * read steps.
 **************************************************************/
static void InitStep2(
    void *timer,       // handle for this timer
    void *priv)        // our private data pointer
{
    ENUM    *pEnum;    // our local info
    DP_PKT   pkt;      // send write and read cmds to the enumerator

    pEnum = (ENUM *) priv;

    // Start with a write to reset the address counter since this
    // daemon might have been started more than once since the FPGA
    // card powered up.

    // Any write to reg 0 in an enumerator resets the address counter
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_NOAUTOINC;
    pkt.core = 0xe0;
    pkt.reg = 0;
    pkt.count = 1;
    pkt.data[0] = 0;            // a write of any data will do
    // Ignore sending status.  Failures are caught by the backup timer
    (void) dpi_tx_pkt(&(pEnum->core[0]), &pkt, 5);

    // Send a read request asking for 255 bytes.
    pkt.cmd = DP_CMD_OP_READ | DP_CMD_NOAUTOINC;
    pkt.core = 0xe0;
    pkt.reg = 0;
    pkt.count = 255;
    // Ignore sending status.  Failures are caught by the backup timer
    (void) dpi_tx_pkt(&(pEnum->core[0]), &pkt, 4);

    pEnum->enumstate = STATE_READING;

    // The state machine to read the contents of the ROM is driven
    // by packet arrival.  If a packet is lost, the state machine
    // hangs.  We run a timer to restart the ROM read in case a packet
    // is lost and the state machine hangs.  5 seconds should be good.
    // Save the timer handle so we can cancel it on ROM read success.
    pEnum->ptimr = add_timer(ED_ONESHOT, 5000, InitStep2, (void *) pEnum);
}


/**************************************************************
 * getSoNames():  - Get the peripheral driver names from the ROM
 **************************************************************/
static int getSoNames(
    ENUM *pEnum)       // Our enum info with ROM image
{
    char    *pc;       // points into the ROM image
    char    *pend;     // last char in the ROM
    int      i;

    pc = pEnum->rom;            // Point to start of ROM
    pend = pc + EROM_SZ;

    // skip over first 9 strings to get to slot 1 driver name
    for (i = 0; i < 9; i++) {
        if (i == 0)
            pEnum->copyright = pc;
        else if (i == 1)
            pEnum->license = pc;
        while (*pc++) {
            if (pc == pend) {
                // Oops, we've scanned past the end of the ROM
                edlog("invalid enumerator ROM");
                return (-1);
            }
        }
    }

    // parse the shared object names
    // (from 1 since we already have the enumerator name in slot 0)
    for (i = 1; i < NUM_CORE; i++) {
        pEnum->core[i].soname = pc;
        while (*pc++) {         // skip to next so name
            if (pc == pend) {
                // Oops, we've scanned past the end of the ROM
                edlog("invalid enumerator ROM");
                return (-1);
            }
        }
    }

    return (0);
}


/***************************************************************************
 *  dpi_tx_pkt():  Send a packet to the board
 *     Return number of bytes sent or -1 on error
 ***************************************************************************/
int dpi_tx_pkt(
    CORE    *pcore,    // The fpga core sending the packet
    DP_PKT  *inpkt,    // The packet to send
    int      len)      // Number of bytes in the packet
{
    unsigned char sltx[DP_PKTLEN]; // SLIP encoded packet
    int      usbfd;    // FD to board's USB serial port
    int      txcount;  // Length of SLIP encoded packet
    int      sntcount; // Number of bytes actually sent

    // sanity check
    if (len < 4) {
        edlog("Invalid packet of length %d from core %d\n", pcore->core_id, len);
        return (-1);
    }

    // Fill in the destination core # and add 'e' in high nibble
    // to help sanity checking down on the board.  Make high
    // nibble of the cmd byte an 'f' to help sanity checking on
    // board too.
    // Note that core is 0 indexed to the peripherals in the FPGA
    // and slot is 0 indexed to the loaded plug-ins.  They might or
    // might not be equal.
    inpkt->cmd  = inpkt->cmd | 0xf0;  // helps error checking
    inpkt->core = inpkt->core | 0xe0;

    // Get and check the USB port's FD from the enumerator core info
    usbfd = ((ENUM *)(pcore->penum))->usbFd;
    if (usbfd == -1) {
        //  not connected to the board.
        return(-1);
    }

    // Convert DP pkt to a SLIP encoded packet
    txcount = dptoslip((unsigned char *) inpkt, len, sltx);

    // print pkts to stdout if debug enabled
    if (DebugMode && (Verbosity == ED_VERB_TRACE)) {
        int      i;
        printf(">>");
        for (i = 0; i < txcount; i++)
            printf(" %02x", sltx[i]);
        printf("\n");
    }

    // write SLIP packet to the USB FD
    sntcount = write(usbfd, sltx, txcount);

    // Check how many bytes were sent.  We get EAGAIN if the USB port
    // buffer is full.  Return an error in this case to let the sender
    // set a timer and try again later (or deal with however).  All
    // other possibilities indicate something more serious -- log it.
    if (sntcount != txcount) {
        if ((sntcount == -1) && (errno != EAGAIN)) {
            edlog("Error sending to FPGA, errno=%d\n", errno);
        }
        return (sntcount);  // return error on partial writes
    }
    return (0);
}

/***************************************************************************
 *  dptoslip():  Convert a DP packet to a SLIP encoded DP packet
 *  Return the number of bytes in the new packet
 ***************************************************************************/
static int dptoslip(
    unsigned char *dppkt,  // The unencode DP packet (input)
    int      len,      // Number of bytes in dppkt
    unsigned char *slppkt) // The SLIP encoded packet (output)
{
    int      dpix = 0; // Index into the input DP packet
    int      slix = 0; // Indes into the output SLIP packet

    // Sanity check on input length
    if (len > DP_PKTLEN)
        return (0);

    // Van Jacobson encoding.  Send opening SLIP_END character
    slppkt[slix++] = SLIP_END;

    // Copy the input packet to the output packet but replace any
    // ESC or END characters with their two character equivalent
    for (dpix = 0; dpix < len; dpix++) {
        if (dppkt[dpix] == SLIP_END) {
            slppkt[slix++] = SLIP_ESC;
            slppkt[slix++] = INPKT_END;
        }
        else if (dppkt[dpix] == SLIP_ESC) {
            slppkt[slix++] = SLIP_ESC;
            slppkt[slix++] = INPKT_ESC;
        }
        else {
            slppkt[slix++] = dppkt[dpix];
        }
    }

    // Send closing SLIP_END character
    slppkt[slix++] = SLIP_END;

    return (slix);
}


/***************************************************************************
 *  receivePkt()  - Handle packets from the FPGA board.
 * 
 ***************************************************************************/
static void receivePkt(
    int      fd,       // FD of USB port with data to read
    void    *priv,     // transparent callback data
    int      rw)       // ==0 on read ready, ==1 on write ready
{
    ENUM    *pEnum;
    unsigned char dppkt[RXBUF_SZ]; // the SLIP decoded packet
    int      dpix;     // index into dppkt
    unsigned char c;   // current char to decode
    int      slstate;  // current state of the decoder
    int      rdret;    // read return value
    int      bufstrt;  // where the current pkt started
    int      bufend;   // number of bytes to process
    int      i;        // buffer loop counter

    pEnum = (ENUM *) priv;

    rdret = read(pEnum->usbFd, &(pEnum->slrx[pEnum->slix]),
        (RXBUF_SZ - pEnum->slix));

    // Was there an error or has the port closed on us?
    if (rdret <= 0) {
        if ((errno != EAGAIN) || (rdret == 0)) {
            edlog(M_NOREAD, pEnum->port);
            exit(-1);
        }
        // EAGAIN means it's recoverable and we just try again later
        return;
    }

    // At this point we have read some bytes from the USB port.  We
    // now scan those bytes looking for SLIP packets.  We put any
    // packets we find into the dppkt buffer and then dispatch the
    // completed packet to the packet handler which routes the packet
    // to the callback registered for that core.
    // Packets with a protocol violation are dropped with a log
    // message.
    // It sometimes happens that a read() returns a full packet and
    // a partial packet.  In this case we process the full packet and
    // move the bytes of the partial packet to the start of the buffer.
    // This way we can always start the SLIP processing at the start 
    // of the buffer.  

    slstate = AWAITING_PKT;
    bufstrt = 0;
    bufend = pEnum->slix + rdret;

    // Drop into a loop to process all the packets in the buffer
    while (1) {
        dpix = 0;               // at start of a new decoded packet
        for (i = bufstrt; i < bufend; i++) {
            c = pEnum->slrx[i];

            // Packets start with the first non-SLIP_END character
            if (slstate == AWAITING_PKT) {
                if (c == SLIP_END)
                    continue;
                else
                    slstate = IN_PACKET; // now in a packet
            }

            if (slstate == IN_PACKET) {
                if (c == SLIP_END) {
                    // Process packet and set up for next one
                    dispatch_packet(pEnum, dppkt, dpix);
                    slstate = AWAITING_PKT;
                    dpix = 0;
                    bufstrt = i; // record start of next packet
                }
                else if (c == SLIP_ESC)
                    slstate = INESCAPE;
                else {
                    // A normal packet byte.  Move it to output packet
                    dppkt[dpix] = c;
                    dpix++;
                }
            }
            else {              // must be INESCAPE
                if (c == INPKT_END) {
                    dppkt[dpix] = SLIP_END;
                    dpix++;
                    slstate = IN_PACKET;
                }
                else if (c == INPKT_ESC) {
                    dppkt[dpix] = SLIP_ESC;
                    dpix++;
                    slstate = IN_PACKET;
                }
                else {          // Protocol violation.  Report it.
                    edlog(M_BADSLIP, pEnum->port);
                    slstate = AWAITING_PKT;
                    dpix = 0;
                }
            }
        }
        if ((bufend - bufstrt - 1) != 0) {
            (void) memmove(pEnum->slrx, &(pEnum->slrx[bufstrt + 1]),
                (bufend - bufstrt - 1));
            pEnum->slix = bufend - bufstrt - 1;
        }
        else {
            pEnum->slix = 0;
        }
        break;                  // Exit the while loop
    }
}


/***************************************************************************
 *  dispatch_packet()  - Verify and route packets to peripheral modules
 ***************************************************************************/
static void dispatch_packet(
    ENUM    *pEnum,       // Board sending the packet
    unsigned char *inbuf, // Points to input packet
    int      len)         // Length of input packet
{
    int      bogus = 0;   // assume it is OK
    DP_PKT  *ppkt;        // maps char pointer to a DP_PKT
    int      pktcore;     // source core for the packet
    int      requested_bytes; // request len and remaining
    int      returned_bytes;
    int      remaining_bytes;
    int      i;

    ppkt = (DP_PKT *) inbuf;
    pktcore = ppkt->core & 0x0f;   // mask high four bits of address

    // Perform as many sanity check as we can before routing this to
    // the driver.

    // Verify minimum packet length
    if (len < 5) {
        bogus = 1;
    }

    // Cmd has to be either a read or a write response
    else if ((ppkt->cmd & DP_CMD_OP_MASK) == 0) {
        bogus = 2;
    }

    // Verify core is in a valid range
    else if (pktcore >= NUM_CORE) {
        bogus = 3;
    }

    // Verify word size, request count, remaining count and len are OK
    // for reads
    else if (ppkt->cmd & DP_CMD_OP_READ) {
        requested_bytes = ppkt->count;
        returned_bytes = len - 5; // four header bytes & remaining
                                  // count
        // Difference between requested and returned should be in
        // remaining
        remaining_bytes = (int) inbuf[len - 1];
        if (remaining_bytes != requested_bytes - returned_bytes) {
            bogus = 4;
        }
    }

    if (bogus != 0)  {
        edlog(M_BADPKT, pEnum->port);
        if (DebugMode && (Verbosity == ED_VERB_TRACE)) {
            printf("<X");
            for (i = 0; i < len; i++)
                printf(" %02x", (unsigned char) (inbuf[i]));
            printf("\n");
        }
        return;
    }

    // print pkts to stdout if debug enabled
    if (DebugMode && (Verbosity == ED_VERB_TRACE)) {
        printf("<<");
        for (i = 0; i < len; i++)
            printf(" %02x", (unsigned char) (inbuf[i]));
        printf("\n");
    }

    // Packet looks OK, dispatch it to the driver if core
    // has registered a received packet callback
    if (pEnum->core[pktcore].pcb) {
        (pEnum->core[pktcore].pcb) (
          &(Slots[pEnum->core[pktcore].slot_id]),  // slot pointer
            ppkt,               // the received packet
            len);               // num bytes in packet
    }
    else {
        // There is no driver for this core and this is an error.
        // However, this is common during start-up since packets can
        // arrive from the FPGA before we've had a chance to register
        // all the peripherals.  Suppress reporting this error  during
        // startup by looking to see if enumerator packet handler is
        // registered.
        if (pEnum->core[0].pcb == NULL) {
            edlog(M_NOSO, pEnum->core[pktcore].core_id, pEnum->port); }
    }
}


/***************************************************************************
 *  initCore()  - Load .so file and call init function for it
 ***************************************************************************/
static void initCore(         // Load and init this core
    CORE    *pcore)
{
    char     driverPath[PATH_MAX]; // Has full name of the .so file
    int      islot;    // which eedd slot this core will use

    // Ignore uninitialized cores or cores with a "null" .so file
    if ((pcore->soname == (char *) 0) ||
        (strcmp("null", pcore->soname) == 0))
        return;

    strncpy(driverPath, pcore->soname, PATH_MAX - 4);
    strcat(driverPath, ".so");

    islot  = add_so(driverPath);
    if (islot >= 0) {
        // We pass to the new peripheral a CORE structure that has its
        // position in the FPGA as well as which enumerator (serial port)
        // it should use for communication to the card.  The peripheral
        // fills in the pointer to the packet arrival callback.  This is
        // how the enumerator know where to send arriving packets.
        Slots[islot].pcore = (void *) pcore;
        pcore->slot_id = islot;

        // Now call the peripheral intialize function
        initslot(&(Slots[islot]));   // run the initializer for the slot
    }
}


// end of enumerator.c
