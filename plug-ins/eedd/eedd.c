/*
 *  Name: eedd.c
 *
 *  Description: Plug-in that gives command line access to daemon status
 *              and configuration.
 *
 *  Resources:
 *              None defined so far.
 */

/*
 * Copyright:   Copyright (C) 2015 by Bob Smith (bsmith@linuxtoys.org)
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

        // What we are is a ...
#define PLUGIN_NAME        "eedd"


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of an eedd plug-in
typedef struct
{
    void    *pslot;    // handle to plug-in's's slot info
} EEDD;


/**************************************************************
 *  - Function prototypes
 **************************************************************/


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    EEDD *pctx;   // our local device context

    // Allocate memory for this plug-in
    pctx = (EEDD *) malloc(sizeof(EEDD));
    if (pctx == (EEDD *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in eedd plug-in initialization");
        return (-1);
    }

    // Init our EEDD structure
    pctx->pslot = pslot;       // this instance of the eedd plug-in

    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->trans = pctx;
    pslot->desc = "Daemon status and configuration";
    pslot->help = README;

    return (0);
}


// end of eedd.c
