/*
 *  Name: gamepad.c
 *
 *  Description: Simple interface to a Linux gamepad device
 *
 *  Resources:
 *    device -  full path to Linux device for the gamepad (/dev/input/js1)
 *    period -  update interval in milliseconds
 *    events -  broadcast for events as they arrive
 *    filter -  disable selected output values
 *    state -   broadcast of total state of gamepad
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
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <limits.h>              // for PATH_MAX
#include <linux/joystick.h>
#include "../include/eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // resource names and numbers
#define FN_DEVICE          "device"
#define FN_PERIOD          "period"
#define FN_EVENTS          "events"
#define FN_FILTER          "filter"
#define FN_STATE           "state"
#define RSC_DEVICE         0
#define RSC_PERIOD         1
#define RSC_EVENTS         2
#define RSC_FILTER         3
#define RSC_STATE          4
        // What we are is a ...
#define PLUGIN_NAME        "gamepad"
        // Default gamepad device
#define DEFDEV             "/dev/input/js1"
        // Maximum size of output string
#define MX_MSGLEN          120
        // Gamepad event size
#define EVENTSZ            (int)sizeof(struct js_event)
        // Number of axis and buttons in state
#define NAXIS              8
#define NBNTN              16


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an gamepad
typedef struct
{
    void    *pslot;    // handle to plug-in's's slot info
    int      period;   // update period for sending state
    void    *ptimer;   // timer with callback to bcast state
    char     device[PATH_MAX]; // full path to gamepad device node
    int      gpfd;     // GamePad File Descriptor (=-1 if closed)
    unsigned char gpevt[EVENTSZ];  // the most recent event
    int      indx;     // index into gpevnt on partial reads
    int      axs[NAXIS];  // current state of axis controls
    int      buttons;  // current state of the buttons
    int      filter;   // filter out event if bit is set
    int      ts;       // timerstame of most recent event
} GAMEPAD;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void getevents(int, void *);
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void sendstate(void *, GAMEPAD *);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    GAMEPAD *pctx;     // our local device context
    int      i;        // loop counter

    // Allocate memory for this plug-in
    pctx = (GAMEPAD *) malloc(sizeof(GAMEPAD));
    if (pctx == (GAMEPAD *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in gamepad initialization");
        return (-1);
    }

    // Init our GAMEPAD structure
    pctx->pslot = pslot;       // this instance of the hello demo
    pctx->period = 0;          // default state update on event
    pctx->filter = 0;          // default is to report all controls
    pctx->indx = 0;            // no bytes in gamepad event structure yet
    (void) strncpy(pctx->device, DEFDEV, PATH_MAX);
    // now open and register the gamepad device
    pctx->gpfd = open(pctx->device, (O_RDONLY | O_NONBLOCK));
    if (pctx->gpfd != -1) {
        add_fd(pctx->gpfd, getevents, (void (*)()) NULL, (void *) pctx);
    }
    pctx->ts = 0;
    pctx->buttons = 0;
    for (i = 0; i < NAXIS; i++) {
        pctx->axs[i] = 0;
    }

    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->priv = pctx;
    pslot->desc = "Gamepad interface";
    pslot->help = README;

    // Add handlers for the user visible resources
    pslot->rsc[RSC_PERIOD].name = FN_PERIOD;
    pslot->rsc[RSC_PERIOD].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_PERIOD].bkey = 0;
    pslot->rsc[RSC_PERIOD].pgscb = usercmd;
    pslot->rsc[RSC_PERIOD].uilock = -1;
    pslot->rsc[RSC_PERIOD].slot = pslot;
    pslot->rsc[RSC_DEVICE].slot = pslot;
    pslot->rsc[RSC_DEVICE].name = FN_DEVICE;
    pslot->rsc[RSC_DEVICE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_DEVICE].bkey = 0;
    pslot->rsc[RSC_DEVICE].pgscb = usercmd;
    pslot->rsc[RSC_DEVICE].uilock = -1;
    pslot->rsc[RSC_EVENTS].name = FN_EVENTS;
    pslot->rsc[RSC_EVENTS].flags = CAN_BROADCAST;
    pslot->rsc[RSC_EVENTS].bkey = 0;
    pslot->rsc[RSC_EVENTS].pgscb = 0;
    pslot->rsc[RSC_EVENTS].uilock = -1;
    pslot->rsc[RSC_EVENTS].slot = pslot;
    pslot->rsc[RSC_FILTER].name = FN_FILTER;
    pslot->rsc[RSC_FILTER].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_FILTER].bkey = 0;
    pslot->rsc[RSC_FILTER].pgscb = usercmd;
    pslot->rsc[RSC_FILTER].uilock = -1;
    pslot->rsc[RSC_FILTER].slot = pslot;
    pslot->rsc[RSC_STATE].name = FN_STATE;
    pslot->rsc[RSC_STATE].flags = CAN_BROADCAST;
    pslot->rsc[RSC_STATE].bkey = 0;
    pslot->rsc[RSC_STATE].pgscb = 0;
    pslot->rsc[RSC_STATE].uilock = -1;
    pslot->rsc[RSC_STATE].slot = pslot;

    // Start the timer to broadcast state info
    if (pctx->period != 0)
        pctx->ptimer = add_timer(ED_PERIODIC, pctx->period, sendstate, (void *) pctx);
    else
        pctx->ptimer = (void *) 0;

    return (0);
}


/**************************************************************
 * usercmd():  - The user is reading or setting one of the configurable
 * resources. 
 **************************************************************/
void usercmd(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    GAMEPAD *pctx;   // our local info
    int      ret;      // return count
    int      nperiod;  // new value to assign to the period
    int      nfilter;  // new value to assign to the filter


    pctx = (GAMEPAD *) pslot->priv;

    if ((cmd == EDGET) && (rscid == RSC_PERIOD)) {
        ret = snprintf(buf, *plen, "%d\n", pctx->period);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_FILTER)) {
        ret = snprintf(buf, *plen, "%05x\n", pctx->filter);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_DEVICE)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->device);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDSET) && (rscid == RSC_PERIOD)) {
        ret = sscanf(val, "%d", &nperiod);
        if ((ret != 1) || (nperiod < 0)) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // record the new period
        pctx->period = nperiod;

        // Delete old timer and create a new one with the new period
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);
        }
        if (pctx->period != 0) {
            pctx->ptimer = add_timer(ED_PERIODIC, pctx->period, sendstate, (void *) pctx);
        }
    }
    else if ((cmd == EDSET) && (rscid == RSC_FILTER)) {
        ret = sscanf(val, "%x", &nfilter);
        if ((ret != 1) || (nfilter > (1 << 24))) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // record the new filter
        pctx->filter = nfilter;
    }
    else if ((cmd == EDSET) && (rscid == RSC_DEVICE)) {
        // Val has the new device path.  Just copy it.
        (void) strncpy(pctx->device, val, PATH_MAX);
        // strncpy() does not force a null.  We add one now as a precaution
        pctx->device[PATH_MAX -1] = (char) 0;
        // close and unregister the old device
        if (pctx->gpfd >= 0) {
            del_fd(pctx->gpfd);
            close(pctx->gpfd);
            pctx->gpfd = -1;
        }
        // now open and register the new device
        pctx->gpfd = open(pctx->device, (O_RDONLY | O_NONBLOCK));
        if (pctx->gpfd != -1) {
            add_fd(pctx->gpfd, getevents, (void (*)()) NULL, (void *) pctx);
        }
    }
    return;
}


/***************************************************************************
 * getevents(): - Read events from the gamepad device
 ***************************************************************************/
static void getevents(
    int       fd_in,         // FD with data to read,
    void     *cb_data)       // callback date (==*GAMEPAD)
{
    GAMEPAD  *pctx;          // our context
    SLOT     *pslot;         // This instance of the gamepad plug-in
    RSC      *prsc;          // pointer to this slot's counts resource
    int       nrd;           // number of bytes read
    int       cindx;         // current indx of byte read so far (usually 0)
    char      msg[MX_MSGLEN+1]; // text to send.  +1 for newline
    int       slen;          // length of text to output
    struct js_event *jsevt;  // to cast gpevt to type struct js_event
    int       mask;          // bit shift variable
    int       bcststate = 0; // broadcast state when set


    pctx = (GAMEPAD *) cb_data;
    cindx = pctx->indx;

    /* We read data from the device until we get the size of a gamepad
     * event.  */
    nrd = read(pctx->gpfd, &(pctx->gpevt[cindx]), (EVENTSZ - cindx));

    // shutdown manager conn on error or on zero bytes read */
    if ((nrd <= 0) && (errno != EAGAIN)) {
        close(pctx->gpfd);
        del_fd(pctx->gpfd);
        pctx->gpfd = -1;
        return;
    }

    // Do we have a full event yet?
    if (cindx + nrd < EVENTSZ) {
        pctx->indx += nrd;   // nope, go wait for more bytes
        return;
    }

    // We have a full event.
    pctx->indx = 0;
    jsevt = (struct js_event *) pctx->gpevt;

    // Broadcast event if any UI are monitoring it.
    pslot = pctx->pslot;
    prsc = &(pslot->rsc[RSC_EVENTS]);  // events resource
    if (prsc->bkey != 0) {
        // write message into a string
        if (jsevt->type == JS_EVENT_BUTTON) {
            slen = snprintf(msg, (MX_MSGLEN +1), "%11d B %d %d\n",
                   jsevt->time, jsevt->number, jsevt->value);
        }
        else if (jsevt->type == JS_EVENT_AXIS) {
            slen = snprintf(msg, (MX_MSGLEN +1), "%11d A %d %d\n",
                   jsevt->time, jsevt->number, jsevt->value);
        }
        // bkey will return cleared if UIs are no longer monitoring us
        bcst_ui(msg, slen, &(prsc->bkey));
    }

    // Update the state info if not filtered
    pctx->ts = jsevt->time;
    if ((jsevt->type == JS_EVENT_AXIS) && (jsevt->number < NAXIS) &&
        (((1 << (jsevt->number + NBNTN)) & pctx->filter) == 0)) {
        pctx->axs[jsevt->number] = jsevt->value;
        bcststate = 1;
    }
    else {
        mask = 1 << jsevt->number;
        if ((jsevt->type == JS_EVENT_BUTTON) && (jsevt->number < NBNTN) &&
            ((mask & pctx->filter) != 0)) {
            if (jsevt->value == 0)
                pctx->buttons = pctx->buttons & ~mask;
            else
                pctx->buttons = pctx->buttons | mask; 
            bcststate = 1;
        }
    }

    // New state is recorded.  Use sendstate() to broadcast it if needed.
    // Don't broadcast state if event is being filtered.
    if (bcststate) {
        sendstate((void *) 0, pctx);
    }

    return;
}


/**************************************************************
 * sendstate():  Send filtered state to broadcast node
 **************************************************************/
void sendstate(
    void     *timer,   // handle of the timer that expired
    GAMEPAD  *pctx)    // Send state to broadcast resource
{
    SLOT     *pslot;   // This instance of the gamepad plug-in
    RSC      *prsc;    // pointer to this slot's counts resource
    char      msg[MX_MSGLEN+1]; // message to send.  +1 for newline
    int       slen;    // length of string to output
    int       i;       // loop counter for axis

    pslot = pctx->pslot;
    prsc = &(pslot->rsc[RSC_STATE]);  // message resource
    // Broadcast state if any UI are monitoring it.
    if (prsc->bkey == 0) {
        return;
    }

    // Write message into a string starting with a timestamp
    slen = snprintf(msg, (MX_MSGLEN +1), "%10d", pctx->ts);

    // Print button values if any button is being monitored.  
    // Buttons are the low 16 bits of the filter (0x00FFFF)
    if ((pctx->filter & 0x00ffff) != 0x00ffff) {   // all filtered?
        slen += snprintf(&(msg[slen]), (MX_MSGLEN +1 -slen), " %04x", pctx->buttons);
    }

    for (i = 0; i < NAXIS; i++) {
        if (((1 << (i + NBNTN)) & pctx->filter) == 0) {
            slen += snprintf(&(msg[slen]), (MX_MSGLEN +1 -slen), " %d", pctx->axs[i]);
        }
    }

    slen += snprintf(&(msg[slen]), (MX_MSGLEN +1 -slen), "\n");

    // bkey will return cleared if UIs are no longer monitoring us
    bcst_ui(msg, slen, &(prsc->bkey));

    return;
}

// end of gamepad.c
