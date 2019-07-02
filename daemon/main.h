/*
 * Name: main.h
 *
 * Description: This file contains the define's and data structures for use
 *              in the empty event-driven daemon
 *
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
 *
 */

#ifndef MAIN_H_
#define MAIN_H_

#include "../plug-ins/include/eedd.h"


/***************************************************************************
 *  - Customization of eedd to use a different name/port
 ***************************************************************************/
#ifndef CPREFIX
#define CPREFIX       "ed"
#endif
#ifndef DEF_UIPORT
#define DEF_UIPORT    8870     /* Default TCP port for ui connections */
#endif


/***************************************************************************
 *  - Defines
 ***************************************************************************/
#define MX_FD           50     /* maximum # of file descriptor in select() call */
#define MX_TIMER        50     /* maximum # of timers */
#define MX_UI           50     /* maximum # of UI connections */

    /* UI sessions are stateful.  Here are the states */
#define CMDSTATE         0     /* waiting for command from UI */
#define RDRPLSTATE       1     /* waiting for hardware reply for a get */
#define MONSTATE         2     /* Monitoring a sensor */
    /* prompt char to signifiy completion of previous command */
#define PROMPT    '\\'



/***************************************************************************
 *  - Data structures  (please see design.txt for more explanation)
 ***************************************************************************/
typedef struct {
    int       cn;              // connection index for this conn
    int       fd;              // FD of TCP conn (=-1 if not in use)
    int       bkey;            // if set, brdcst data from this slot/rsc
    int       o_port;          // Other-end TCP port number
    int       o_ip;            // Other-end IP address
    int       cmdindx;         // Index of next location in cmd buffer
    char      cmd[MXCMD];      // command from UI program
} UI;

    /* the information kept for each file descriptor callback */
typedef struct {
    int       fd;              // FD of TCP conn (=-1 if not in use)
    int       stype;           // OR of ED_ READ, WRITE, and EXCEPT
    void      (*scb) ();       // Callback on select() activity
    void     *pcb_data;        // data included in call of callbacks
} ED_FD;

    /* structure for the timers and their callbacks */
typedef struct {
    int       type;            // one-shot, periodic, or unused
    long long to;              // us since Jan 1, 1970 to timeout
    unsigned int us;           // period or timeout interval
    void      (*cb) ();        // Callback on timeout
    void     *pcb_data;        // data included in call of callbacks
} ED_TIMER;




/***************************************************************************
 *  - Function prototypes
 ***************************************************************************/


#endif /*MAIN_H_*/


