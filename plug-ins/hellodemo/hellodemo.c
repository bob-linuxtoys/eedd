/*
 *  Name: hellodemo.c
 *
 *  Description: Simple demonstration plug-in for the empty daemon
 *
 *  Resources:
 *    messagetext - the text of the message to broadcast (edget, edset)
 *    message - broadcast resource to hear message every period seconds (edcat)
 *    period  - update interval in seconds (edget, edset)
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
#include "../include/eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // resource names and numbers
#define FN_TEXT            "messagetext"
#define FN_PERIOD          "period"
#define FN_MESSAGE         "message"
#define RSC_TEXT           0
#define RSC_PERIOD         1
#define RSC_MESSAGE        2
        // Maximum message length
#define MX_MSGLEN          60
        // What we are is a ...
#define PLUGIN_NAME        "hello_demo"


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an hellodemo
typedef struct
{
    void    *pslot;    // handle to plug-in's's slot info
    int      period;   // update period for sending text to message
    void    *ptimer;   // timer with callback to bcast text
    char     text[MX_MSGLEN]; // text to send every period seconds
} HELLODEMO;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void sendnow(void *, HELLODEMO *);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    HELLODEMO *pctx;   // our local device context

    // Allocate memory for this plug-in
    pctx = (HELLODEMO *) malloc(sizeof(HELLODEMO));
    if (pctx == (HELLODEMO *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in hellodemo initialization");
        return (-1);
    }

    // Init our HELLODEMO structure
    pctx->pslot = pslot;       // this instance of the hello demo
    pctx->period = 1;          // default message output period
    (void) strncpy(pctx->text, "Hello, World!", MX_MSGLEN);

    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->priv = pctx;
    pslot->desc = "Hello,World Demo Plug-in";
    pslot->help = README;
    // Add handlers for the user visible resources
    pslot->rsc[RSC_PERIOD].name = FN_PERIOD;
    pslot->rsc[RSC_PERIOD].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_PERIOD].bkey = 0;
    pslot->rsc[RSC_PERIOD].pgscb = usercmd;
    pslot->rsc[RSC_PERIOD].uilock = -1;
    pslot->rsc[RSC_TEXT].slot = pslot;
    pslot->rsc[RSC_TEXT].name = FN_TEXT;
    pslot->rsc[RSC_TEXT].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_TEXT].bkey = 0;
    pslot->rsc[RSC_TEXT].pgscb = usercmd;
    pslot->rsc[RSC_TEXT].uilock = -1;
    pslot->rsc[RSC_PERIOD].slot = pslot;
    pslot->rsc[RSC_MESSAGE].name = FN_MESSAGE;
    pslot->rsc[RSC_MESSAGE].flags = CAN_BROADCAST;
    pslot->rsc[RSC_MESSAGE].bkey = 0;
    pslot->rsc[RSC_MESSAGE].pgscb = 0;
    pslot->rsc[RSC_MESSAGE].uilock = -1;
    pslot->rsc[RSC_MESSAGE].slot = pslot;

    // Start the timer to send the message text
    pctx->ptimer = add_timer(ED_PERIODIC, (pctx->period * 1000), sendnow, (void *) pctx);

    return (0);
}


/**************************************************************
 * usercmd():  - The user is reading or setting one of the configurable
 * resources.  (I like doing this all in one routine with if() to
 * separate out the different cases.  You can use separate routines
 * for each resource if you wish.)
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
    HELLODEMO *pctx;   // our local info
    int      ret;      // return count
    int      nperiod;  // new value to assign the period

    pctx = (HELLODEMO *) pslot->priv;

    if ((cmd == EDGET) && (rscid == RSC_PERIOD)) {
        ret = snprintf(buf, *plen, "%d\n", pctx->period);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_TEXT)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->text);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDSET) && (rscid == RSC_PERIOD)) {
        ret = sscanf(val, "%d", &nperiod);
        if ((ret != 1) || (nperiod < 1)) {  // one value greater than 0
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        // record the new period
        pctx->period = nperiod;

        // Delete old timer and create a new one with the new period
        del_timer(pctx->ptimer);
        pctx->ptimer = add_timer(ED_PERIODIC, (pctx->period * 1000), sendnow, (void *) pctx);
    }
    else if ((cmd == EDSET) && (rscid == RSC_TEXT)) {
        // Val has the new message.  Just copy it.
        // Strings longer than MX_MSGLEN are silently truncated
        (void) strncpy(pctx->text, val, MX_MSGLEN);
        // strncpy() does not force a null.  We add one now as a precaution
        pctx->text[MX_MSGLEN -1] = (char) 0;
    }
    return;
}


/**************************************************************
 * sendnow():  Send message to broadcast node
 **************************************************************/
void sendnow(
    void      *timer,  // handle of the timer that expired
    HELLODEMO *pctx)   // Send message to broadcast resource
{
    SLOT     *pslot;   // This instance of the hellodemo plug-in
    RSC      *prsc;    // pointer to this slot's counts resource
    char      msg[MX_MSGLEN+1]; // message to send.  +1 for newline
    int       slen;    // length of string to output

    pslot = pctx->pslot;
    prsc = &(pslot->rsc[RSC_MESSAGE]);  // message resource
    // Broadcast it if any UI are monitoring it.
    if (prsc->bkey == 0) {
        return;
    }

    // write message into a string
    slen = snprintf(msg, (MX_MSGLEN +1), "%s\n", pctx->text);

    // bkey will return cleared if UIs are no longer monitoring us
    bcst_ui(msg, slen, &(prsc->bkey));

    return;
}

// end of hellodemo.c
