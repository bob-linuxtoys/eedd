/*
 * Name: main.c
 * 
 * Description: This is the entry point for the empty daemon
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

/*
 * 
 * edserver Program Flow:
 *   -- Process command line options
 *   -- Init
 *   -- Load command line .so files (-s option)
 *   -- If no Slot 0 .so file, then load eedd.so file
 *   -- Walk the list of .so files doing the init() routine for each
 *   -- Main loop
 * 
 * Synopsis: eedd [options]
 *   
 * options:
 *  -e, --stderr           Route messages to stderr instead of log even if running in
 *                         background (i.e. no stderr redirection).
 *  -v, --verbosity        Set the verbosity level of messages: 0 (errors), 1 (+debug),
 *                         2 (+ warnings), or 3 (+ info), default = 0.
 *  -d, --debug            Enable debug mode.
 *  -f, --foreground       Stay in foreground.
 *  -a, --listen_any       Listen for incoming UI connections on any IP address
 *  -p, --listen_port      Listen for incoming UI connections on this port
 *  -r, --realtime         Try to run with real-time extensions.
 *  -V, --version          Print version number and exit.
 *  -s, --slot             Load .so file for slot specified, as slotID:file.1.
 *  -h, --help             Print usage message
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/time.h>
#include <sys/mman.h>
#include "main.h"

/***************************************************************************
 *  - Limits and defines
 ***************************************************************************/


/***************************************************************************
 *  - Function prototypes
 ***************************************************************************/
static void globalinit();
static void daemonize();
static void invokerealtimeextensions();
static void processcmdline(int, char *[]);
extern void open_ui_port();
extern void muxmain();
extern void initslot(SLOT *);  // Load and init this slot
extern int  add_so(char *);


/***************************************************************************
 *  - System-wide global variable allocation
 ***************************************************************************/
SLOT     Slots[MX_PLUGIN];     // Allocate the plug-in table
ED_FD    Ed_Fd[MX_FD];         // Table of open FDs and callbacks
ED_TIMER Timers[MX_TIMER];     // Table of timers
UI       UiCons[MX_UI];        // Table of UI connections
int      UseStderr = 0; // use stderr
int      Verbosity = 0; // verbosity level
int      DebugMode = 0; // run in debug mode
int      UiaddrAny = 0; // Use any IP address if set
int      UiPort = DEF_UIPORT; // TCP port for ui connections
int      ForegroundMode = 0; // run in foreground
int      RealtimeMode = 0; // use realtime extension


/***************************************************************************
 *  - Main.c specific globals
 ***************************************************************************/
char    *CmdName;      // How this program was invoked
const char *versionStr = "eedd Version 0.9.0, Copyright 2019 by Demand Peripherals, Inc.";
const char *usageStr = "usage: eedd [-ev[level]dfrVmsh]\n";
const char *helpText = "\
eedd [options] \n\
 options:\n\
 -e, --stderr            Route messages to stderr instead of log even if running in\n\
                         background (i.e. no stderr redirection).\n\
 -v, --verbosity         Set the verbosity level of messages: 0 (errors), 1 (+debug),\n\
                         2 (+ warnings), or 3 (+ info), default = 0.\n\
 -d, --debug             Enable debug mode.\n\
 -f, --foreground        Stay in foreground.\n\
 -a, --listen_any        Use any/all IP addresses for UI TCP connections\n\
 -p, --listen_port       Listen for incoming UI connections on this TCP port\n\
 -r, --realtime          Try to run with real-time extensions.\n\
 -V, --version           Print version number and exit.\n\
 -s, --slot              Load .so.X file for slot specified, as slotID:file.so\n\
 -h, --help              Print usage message.\n\
";


/***************************************************************************
 *  - main():  It all begins here
 ***************************************************************************/
int main(int argc, char *argv[])
{
    int     i;      // loop counter

    // Ignore the SIGPIPE signal since that can occur if a
    // UI socket closes just before we try to write to it.
    (void) signal(SIGPIPE, SIG_IGN);

    // Initialize globals for slots, timers, ui connections, and select fds
    globalinit();

    // Add plug-ins here to always have them when the program starts
    // The first loaded is in slot 0, the next in slot 1, ...
    (void) add_so("serial_fpga.so");         // slot 0
    (void) add_so("hba_basicio.so");         // slot 1
    (void) add_so("hba_qtr.so");             // slot 2
    (void) add_so("hba_motor.so");           // slot 3
    (void) add_so("hba_sonar.so");           // slot 4 


    // Parse the command line and set global flags 
    processcmdline(argc, argv);
    (void) umask((mode_t) 000);

    // Become a daemon
    if (!ForegroundMode)
        daemonize();

    // invoke real-time extensions if specified
    if (RealtimeMode)
        invokerealtimeextensions();

    // Start eedd and the plug-ins loaded from the command line 
    for (i = 0; i < MX_PLUGIN; i++) {
        initslot(&(Slots[i]));
    }

    // Open the TCP listen port for UI connections
    open_ui_port();

    // Drop into the select loop and wait for events
    muxmain();

    return (0);
}


/***************************************************************************
 *  globalinit()   Initialize the global arrays
 *      Explicitly initializing every field is more part of the documentation
 *  than part of the code.
 ***************************************************************************/
void globalinit()
{
    int    i,j;    // loop counters

    // Initialize the plug-in table
    for (i = 0; i < MX_PLUGIN; i++) {
        Slots[i].slot_id = i;
        Slots[i].name    = (char *) NULL; // functional name of plug-in
        Slots[i].soname[0] = (char) 0;    // so file name
        Slots[i].handle  = (void *) NULL;
        Slots[i].priv    = (void *) NULL;
        Slots[i].desc    = (void *) NULL;
        Slots[i].help    = (void *) NULL;
        for (j = 0; j < MX_RSC; j++) {
            Slots[i].rsc[j].name   = (char *) NULL;
            Slots[i].rsc[j].pgscb  = NULL;
            Slots[i].rsc[j].slot   = (void *) NULL;
            Slots[i].rsc[j].bkey   = 0;
            Slots[i].rsc[j].uilock = 0;
            Slots[i].rsc[j].flags  = 0;
        }
    }

    for (i = 0; i < MX_FD; i++) {
        Ed_Fd[i].fd       = -1;
        Ed_Fd[i].stype    = 0;    // read, write, or except
        Ed_Fd[i].scb      = NULL; // callback on select() activity
        Ed_Fd[i].pcb_data = (void *) NULL; // data included in call of callback
    }

    for (i = 0; i < MX_TIMER; i++) {
        Timers[i].type     = ED_UNUSED;
        Timers[i].type     = 0;           // one-shot, periodic, or unused
        Timers[i].to       = (long long) 0; // ms since Jan 1, 1970 to timeout
        Timers[i].us       = 0;           // period or timeout interval in uS
        Timers[i].cb       = NULL;        // Callback on timeout
        Timers[i].pcb_data = (void *) NULL; // data included in call of callbacks
    }

    for (i = 0; i <MX_UI; i++) {
        UiCons[i].cn = i;                 // Record index in struct
        UiCons[i].fd = -1;                // fd=-1 says ui is not in use
        UiCons[i].bkey = 0;               // if set, brdcst data from this slot/rsc
        UiCons[i].o_port = 0;             // Other-end TCP port number
        UiCons[i].o_ip = 0;               // Other-end IP address
        UiCons[i].cmdindx = 0;            // Index of next location in cmd buffer
        UiCons[i].cmd[0] = (char) 0;      // command from UI program
    }
}


/***************************************************************************
 *  processcmdline()   Process the command line
 ***************************************************************************/
void processcmdline(int argc, char *argv[])
{
    int      c;
    int      optidx = 0;
    static struct option longoptions[] = {
        {"stderr", 0, 0, 'e'},
        {"verbosity", 1, 0, 'v'},
        {"debug", 0, 0, 'd'},
        {"foreground", 0, 0, 'f'},
        {"realtime", 0, 0, 'r'},
        {"version", 0, 0, 'V'},
        {"listen_any", 0, 0, 'a'},
        {"listen_port", 1, 0, 'p'},
        {"slot", 1, 0, 's'},
        {"help", 0, 0, 'h'},
        {0, 0, 0, 0}
    };
    static char optStr[] = "ev:dfrVs:p:ah";

    while (1) {
        c = getopt_long(argc, argv, optStr, longoptions, &optidx);
        if (c == -1)
            break;

        switch (c) {
            case 'e':
                UseStderr = 1;
                break;

            case 'v':
                Verbosity = atoi(optarg);
                break;

            case 'd':
                DebugMode = 1;
                break;

            case 'f':
                ForegroundMode = 1;
                break;

            case 'a':
                UiaddrAny = 1;
                break;

            case 'p':
                UiPort = atoi(optarg);
                break;

            case 'r':
                RealtimeMode = 1;
                break;

            case 'V':
                printf("%s\n", versionStr);
                exit(-1);
                break;

            case 's':
                add_so(optarg);
                break;

            default:
                printf("%s", helpText);
                exit(-1);
        }
    }

    // At this point we've gotten all of the command line options.
    // Save the command line name for this program
    CmdName = argv[0];
}

/***************************************************************************
 *  Become a daemon
 ***************************************************************************/
void daemonize()
{
    pid_t    dpid;
    int      maxFd;
    int      fd;
    int      i;

    // go into the background
    if ((dpid = fork()) < 0) {
        edlog(M_NOFORK, strerror(errno));
        exit(-1);
    }
    if (dpid > 0) {
        // exit if parent
        exit(0);
    }

    // become process and session leader
    if ((dpid = setsid()) < 0) {
        edlog(M_NOSID, strerror(errno));
        exit(-1);
    }

    // set the current directory
    if (chdir("/") < 0) {
        edlog(M_NOCD, strerror(errno));
        exit(-1);
    }

    // redirect stio to /dev/null
    close(STDIN_FILENO);
    fd = open("/dev/null", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        edlog(M_NONULL, strerror(errno));
        exit(-1);
    }
    else if (fd != 0) {
        // no stdio
        edlog(M_NOREDIR, "stdin");
        exit(-1);
    }
    close(STDOUT_FILENO);
    fd = open("/dev/null", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        edlog(M_NONULL, strerror(errno));
        exit(-1);
    }
    else if (fd != 1) {
        // no stdio
        edlog(M_NOREDIR, "stdout");
        exit(-1);
    }
    if (!UseStderr) {
        // do not redirect if forced to use stderr
        close(STDERR_FILENO);
        fd = open("/dev/null", O_RDONLY | O_NOCTTY);
        if (fd < 0) {
            edlog(M_NONULL, strerror(errno));
            exit(-1);
        }
        else if (fd != 2) {
            // no stdio
            edlog(M_NOREDIR, "stderr");
            exit(-1);
        }
    }

    // close all non-stdio
    maxFd = getdtablesize();
    for (i = 3; i < maxFd; i++) {
        close(i);
    }

    // reset the file modes
    (void) umask((mode_t) 000);
}

/***************************************************************************
 *  Give the daemon the highest scheduling priority possible
 ***************************************************************************/
void invokerealtimeextensions()
{
    struct sched_param sp;
    int      policy;

    // change the static priority to the highest possible and set FIFO
    // scheduling
    if ((pthread_getschedparam(pthread_self(), &policy, & sp) != 0)) {
        edlog(M_BADSCHED, strerror(errno));
    }
    if (policy == SCHED_OTHER) {
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
            edlog(M_BADSCHED, strerror(errno));
        }
    }

    // lock all current and future memory pages
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1) {
        edlog(M_BADMLOCK, strerror(errno));
    }
}

// end of main.c

