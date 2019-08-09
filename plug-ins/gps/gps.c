/*
 *  Name: gps.c
 *
 *  Description: Minimal driver for a GPS receiver
 *
 *  The gps driver connects to a serial port and watches for incoming
 *  GPS sentences.  It uses the GGA sentence to get the status, time,
 *  and location of the GPS receiver.
 *    This peripheral does not use the FPGA.  It is included both as
 *  an sample non-FPGA peripheral and as a simple GPS decoder.
 *
 *  Resources:
 *    config    - the baud rate and serial port to the GPS receiver
 *    status    - the state of the receiver and number of satellites in use
 *    tll       - time, longitude, and latitude
 *
 */

/*
 *
 * Copyright:   Copyright (C) 2018 Demand Peripherals, Inc.
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
#include <termios.h>
#include <ctype.h>
#include "../include/eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // misc constants
#define GPS_STR_LEN         100
        // Resources
#define RSC_CONFIG          0
#define RSC_STATUS          1
#define RSC_TLL             2
        // NEMA sentence GGA field locations
        //$GPGGA,191611.565,3722.6843,N,12159.1424,W,0,00,50.0,13.9,M,,M,,0000*56
#define GGA_TIME            0
#define GGA_LAT             1
#define GGA_NS              2
#define GGA_LONG            3
#define GGA_EW              4
#define GGA_QUALITY         5
#define GGA_NSAT            6
#define GGA_CHECKSUM        14
#define GGA_NUM_FIELD       15


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an gps
typedef struct
{
    void    *pslot;    // handle to peripheral's slot info
    int      gpsfd;    // FD to the serial port
    char     port[GPS_STR_LEN];  // /dev/ entry for serial port
    int      baudrate; // of the serial port to the GPS  
    int      status;   // most recent status
    int      nsat;     // most recent satellite count
    char     linein[GPS_STR_LEN];  // string from GPS receiver
    int      ininx;    // index into linein
} GPSDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void gpscb(int, void *, int);
static void gpsuser(int, int, char*, SLOT*, int, int*, char*);
static void do_nema(GPSDEV  *);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    GPSDEV   *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (GPSDEV *) malloc(sizeof(GPSDEV));
    if (pctx == (GPSDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in gps initialization");
        return (-1);
    }

    // Init our GPSDEV structure
    pctx->pslot = pslot;       // out instance of a peripheral
    pctx->gpsfd = -1;          // an FD of -1 is not valid
    pctx->status = -1;         // serial port not open or in error
    pctx->nsat = 0;            // no satellites in use
    pctx->ininx = 0;           // no chars from receiver yet
    strncpy(pctx->port, "(null)", 7);  // 7==strlen("null") + 1 for null

    // Register this slot's private data
    pslot->name = "gps";
    pslot->desc = "GPS Decoder";
    pslot->help = README;
    pslot->priv = pctx;
    // Add the handlers for the user visible resources
    pslot->rsc[RSC_CONFIG].name = "config";
    pslot->rsc[RSC_CONFIG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CONFIG].bkey = 0;
    pslot->rsc[RSC_CONFIG].pgscb = gpsuser;
    pslot->rsc[RSC_CONFIG].uilock = -1;
    pslot->rsc[RSC_CONFIG].slot = pslot;
    pslot->rsc[RSC_STATUS].name = "status";
    pslot->rsc[RSC_STATUS].flags = IS_READABLE;
    pslot->rsc[RSC_STATUS].bkey = 0;
    pslot->rsc[RSC_STATUS].pgscb = gpsuser;
    pslot->rsc[RSC_STATUS].uilock = -1;
    pslot->rsc[RSC_STATUS].slot = pslot;
    pslot->rsc[RSC_TLL].name = "tll";
    pslot->rsc[RSC_TLL].flags = CAN_BROADCAST;
    pslot->rsc[RSC_TLL].bkey = 0;
    pslot->rsc[RSC_TLL].pgscb = gpsuser;
    pslot->rsc[RSC_TLL].uilock = -1;
    pslot->rsc[RSC_TLL].slot = pslot;


    return (0);
}


/**************************************************************
 * gpsuser():  - The user is reading or writing the GPS config.
 **************************************************************/
void gpsuser(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    GPSDEV  *pctx;     // our local info
    int      ret;      // return count
    int      newbaud;  // new serial port baud rate
    char     newport[GPS_STR_LEN];
    int      baudflag; // BXXXXXX for the baudrate
    struct termios tbuf;


    pctx = (GPSDEV *) pslot->priv;

    // print individual pulse width
    if ((cmd == EDGET) && (rscid == RSC_CONFIG)) {
        ret = snprintf(buf, *plen, "%d %s\n", pctx->baudrate, pctx->port);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDGET) && (rscid == RSC_STATUS)) {
        ret = snprintf(buf, *plen, "%d %d\n", pctx->status, pctx->nsat);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if ((cmd == EDSET) && (rscid == RSC_CONFIG)) {
        ret = sscanf(val, "%d %99s", &newbaud, newport);  // !!!! 99 is GPS_STR_LEN - 1
        // baudrate must be one of the common values
        if ((ret != 2) || 
            ((newbaud != 1200) && (newbaud != 2400) &&
             (newbaud != 4800) && (newbaud != 9600) &&
             (newbaud != 19200) && (newbaud != 38400) &&
             (newbaud != 115200)))
        {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // Valid new baudrate and serial port
        strncpy(pctx->port, newport, GPS_STR_LEN);
        pctx->baudrate = newbaud;

        // setting the config causes us to close and reopen the serial port
        if (pctx->gpsfd >= 0) {
            del_fd(pctx->gpsfd);
            close(pctx->gpsfd);
            pctx->status = -1;
        }
        pctx->gpsfd = open(pctx->port, O_RDWR | O_NONBLOCK, 0);
        if (pctx->gpsfd < 0) {
            // bummer, we could not open the serial port
            // return and let the user try again later
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // set the baud rate.  Close port on any error
        baudflag = (newbaud == 1200) ? B1200 :
                   (newbaud == 2400) ? B2400 :
                   (newbaud == 4800) ? B4800 :
                   (newbaud == 9600) ? B9600 :
                   (newbaud == 19200) ? B19200 :
                   (newbaud == 38400) ? B38400 :
                   (newbaud == 115200) ? B115200 : 0;
        tbuf.c_cflag = CS8 | CREAD | baudflag | CLOCAL;
        tbuf.c_iflag = IGNBRK;
        tbuf.c_oflag = 0;
        tbuf.c_lflag = 0;
        tbuf.c_cc[VMIN] = 1;        /* character-by-character input */
        tbuf.c_cc[VTIME] = 0;       /* no delay waiting for characters */
        int actions = TCSANOW;
        if (tcsetattr(pctx->gpsfd, actions, &tbuf) < 0) {
            close(pctx->gpsfd);
            pctx->gpsfd = -1;
            pctx->status = -1;
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // Open and configure of serial port worked.  Set the
        // read callback to get the GPS sentences.
        pctx->status = 0;
        pctx->ininx = 0;
        add_fd(pctx->gpsfd, ED_READ, gpscb, pctx);
    }

    *plen = 0;    // nothing to send to the user
    return;
}


/***************************************************************************
 *  gpscb()  - Handle serial data from the gps receiver
 *
 ***************************************************************************/
void gpscb(
    int      fd,       // FD of device with data ready to read
    void    *priv,     // transparent callback data
    int rw)            // ==0 on read ready, ==1 on write ready
{
    GPSDEV  *pctx;     // our local info
    int      ret;      // return status
    int      i;        // loop index


    pctx = (GPSDEV *) priv;  // get our context


    ret = read(fd, &(pctx->linein[pctx->ininx]), (GPS_STR_LEN - pctx->ininx));
    // error out with a log message if read error
    if (ret == -1) {
        if (errno == EAGAIN)
            return;                  // to be immediately called again
        del_fd(fd);
        pctx->gpsfd = -1;
        pctx->status = -1;
        edlog(M_NOREAD, pctx->port);
        return;
    }
    else if (ret == 0) {             // done with read
        return;
    }
    else if (pctx->ininx + ret == GPS_STR_LEN) {
        // buffer overflow,  Reset pointer to discard bogus line
        pctx->ininx = 0;
        return;
    }
    pctx->ininx = pctx->ininx + ret;


    // NEMA sentences are in the buffer. Call the parser to process them
    // Scan for a newline.    If found, replace it with a null and process line
    // Lines are terminated with \r\n but the \r does not cause a problem
    for (i = 0; i < pctx->ininx; i++) {
        if (pctx->linein[i] == '\n') {
            pctx->linein[i] = (char) 0;
            do_nema(pctx);
            (void) memmove(pctx->linein, &(pctx->linein[i+1]), (pctx->ininx - (i+1)));
            pctx->ininx -= i+1;
            i = 0;
        }
    }

    return;
}


/***************************************************************************
 *  do_nema()  - process a line from the GPS receiver
 *
 ***************************************************************************/
void do_nema(
    GPSDEV   *pctx)    // our local info
{
    SLOT     *pslot;
    RSC      *prsc;    // pointer to this slot's counts resource
    int       notgga;  // == 0 if a $GPGGA sentence
    int       sum;     // xor checksum
    int       i;
    char     *fld[GGA_NUM_FIELD];  // GGA should have _exactly_ 15 fields
    int       j = 0; // index into fld[];
    char      lineout[GPS_STR_LEN];  // output to send to users
    int       nout;    // length of output line
    int       tmpi;    // temp int
    double    tmpd;    // temp double
    double    lng;     // longitude
    double    lat;     // latitude
    int       midnightsecs; // number of seconds since midnight UTC
    int       nconv = 0; // number of valid conversions


    // Get slot and pointer to TLL resource structure
    pslot = pctx->pslot;
    prsc = &(pslot->rsc[RSC_TLL]);


    // We only process the GGA sentences.  Return if anything else
    notgga = strncmp("$GPGGA,", pctx->linein, 7);  // 7=strlen($GPGGA,)
    if (notgga) {
        return;
    }

    // Prepare line for processing and verify checksum. 
    // We replace the commas with a null and note the location of the next char
    sum = 0;
    for (i = 1; i < pctx->ininx; i++) {
        if (pctx->linein[i] == '*') {
            pctx->linein[i] = (char) 0;
            fld[j] = &(pctx->linein[i+1]);
            j++;
            // Sanity check number of fields
            if (j != GGA_NUM_FIELD) {       // must be exactly 15
                return;                     // bogus line, ignore it
            }
            break;                          // checksum is last field
        }
        sum = sum ^ pctx->linein[i];
        if (pctx->linein[i] == ',') {
            pctx->linein[i] = (char) 0;
            fld[j] = &(pctx->linein[i+1]);
            j++;
            // Sanity check number of fields
            if (j == GGA_NUM_FIELD) {       // Too many fields?
                return;                     // bogus line, ignore it
            }
        }
    }

    // Checksum is the two chars after the '*' and includes the xor of
    // everything from GPGGA... up to the *.  Verify checksum.
    sscanf(fld[GGA_CHECKSUM], "%x", &tmpi);
    if (tmpi != sum) {
        return;
    }

    // An NEMA GGA sentence with a valid checksum and the right number of
    // fields in the line.  Extract and save status info.
    nconv += sscanf(fld[GGA_NSAT], "%d", &(pctx->nsat));   // get sat count
    nconv += sscanf(fld[GGA_QUALITY], "%d", &tmpi); // tmpi is 0 if no lock
    pctx->status = (tmpi == 0) ? 0 : 1;

    // rest of the data is bogus if no satellite lock
    if (tmpi == 0) {
        return;
    }

    // All that remains is to format the data and send it to listeners.
    // Just return if no one has dpcat'ed tll
    if (prsc->bkey == 0) {
        return;
    }

    nconv += sscanf(fld[GGA_TIME], "%d", &tmpi);   // tmpi is HHMMSS format
    midnightsecs = (tmpi / 10000) * 3600 +             // HH
                   ((tmpi / 100) % 100 ) * 60 +        // MM
                   (tmpi % 100);                       // SS

    nconv += sscanf(fld[GGA_LAT], "%lf", &tmpd);   // tmpd is DDDMM.mmm format
    lat = (double)((int)tmpd / 100);                  // DDD
    lat = lat + ((tmpd - (lat * 100.0)) / 60.0);                   // +MM.mmm degrees
    // Latitude is north/south based on the GGA_NS field
    lat = ( *fld[GGA_NS] == 'S') ? -lat : lat;

    nconv += sscanf(fld[GGA_LONG], "%lf", &tmpd);  // tmpd is DDDMM.mmm format
    lng = (double)((int)tmpd / 100);                  // DDD
    lng = lng + ((tmpd - (lng * 100.0)) / 60.0);                   // +MM.mmm degrees
    // Longitude is east/west based on the GGA_EW field
    lng = ( *fld[GGA_EW] == 'W') ? -lng : lng;

    // final check is on the number of sscanf conversion
    if (nconv != 5) {
        return;
    }

    snprintf(lineout, GPS_STR_LEN, "%d %9.4lf %9.4lf\n", midnightsecs, lat, lng);
    nout = strnlen(lineout, GPS_STR_LEN-1);
    // bkey will return cleared if UIs are no longer monitoring us
    bcst_ui(lineout, nout, &(prsc->bkey));

    return;
}




