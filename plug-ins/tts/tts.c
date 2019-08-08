/*
 *  Name: tts.c
 *
 *  Description: Text-to-speech using CMU Festival Lite, flite
 *
 *  Resources:
 *    voice   - which voice to use when speaking (edget, edset)
 *    speak   - the word to speak (edset)
 *    status  - Whether the system is busy or not (edget, edcat)
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

/*
 * Design notes:
 * The event system in eedd is based on a select() call.  Since we
 * want to know when the flite child process is done we need a way
 * to make its status visible on a file descriptor.
 *   We could use eventfd but we want something that is only visible
 * inside our plug-in and is not visible, or affects, the whole program.
 *   We spawn a process and then open a pipe to that process.  That
 * process (child1) then does a fork/exec to run the actual flite
 * command as child2.  Child1 waits until the flite program is done
 * and then writes the flite exit code to its end of the pipe.
 *  A read callback on the eedd end of the pipe is invoked, reads
 * the flite exit code, and then does a waitpid to finish closing
 * child1.
 */


#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include "../include/eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // resource names and numbers
#define FN_VOICE          "voice"
#define FN_SPEAK          "speak"
#define FN_STATUS         "status"
#define RSC_VOICE         0
#define RSC_SPEAK         1
#define RSC_STATUS        2
        // Maximum message length
#define MX_MSGLEN          60
        // What we are is a ...
#define PLUGIN_NAME        "tts"
        // longest lenght of a voice
#define VOICELEN          10
        // length of maximum line 
#define MX_LINE           1000


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of a tts peripheral
typedef struct
{
    void    *pslot;    // handle to plug-in's's slot info
    pid_t    child1;   // timer with callback to bcast text
    char     voice[MX_MSGLEN]; // which voice to use when speaking
    int      pipefd[2]; // first fd is read side of pipe
} TTS;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void speak_complete(int fd, TTS  *pctx);
extern int pipe2(int __pipedes[2], int __flags);

/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    TTS *pctx;   // our local device context

    // Allocate memory for this plug-in
    pctx = (TTS *) malloc(sizeof(TTS));
    if (pctx == (TTS *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in tts initialization");
        return (-1);
    }

    // Init our TTS structure
    pctx->pslot = pslot;             // this instance of the tts
    pctx->child1 = (pid_t) -1;       // no child process yet
    (void) strncpy(pctx->voice, "slt", MX_MSGLEN);

    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->priv = pctx;
    pslot->desc = "Text-To-Speech Peripheral";
    pslot->help = README;
    // Add handlers for the user visible resources
    pslot->rsc[RSC_SPEAK].name = FN_SPEAK;
    pslot->rsc[RSC_SPEAK].flags = IS_WRITABLE;
    pslot->rsc[RSC_SPEAK].bkey = 0;
    pslot->rsc[RSC_SPEAK].pgscb = usercmd;
    pslot->rsc[RSC_SPEAK].uilock = -1;
    pslot->rsc[RSC_VOICE].slot = pslot;
    pslot->rsc[RSC_VOICE].name = FN_VOICE;
    pslot->rsc[RSC_VOICE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_VOICE].bkey = 0;
    pslot->rsc[RSC_VOICE].pgscb = usercmd;
    pslot->rsc[RSC_VOICE].uilock = -1;
    pslot->rsc[RSC_SPEAK].slot = pslot;
    pslot->rsc[RSC_STATUS].name = FN_STATUS;
    pslot->rsc[RSC_STATUS].flags = CAN_BROADCAST | IS_READABLE;
    pslot->rsc[RSC_STATUS].bkey = 0;
    pslot->rsc[RSC_STATUS].pgscb = usercmd;
    pslot->rsc[RSC_STATUS].uilock = -1;
    pslot->rsc[RSC_STATUS].slot = pslot;

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
    TTS     *pctx;     // our local info
    int      ret = 0;  // return count
    pid_t    child2;   // PID of second child process
    char     tmpbuf[MX_LINE];      // utility string
    int      wstatus;  // return status of the actual flite command


    pctx = (TTS *) pslot->priv;


    if ((cmd == EDGET) && (rscid == RSC_VOICE)) {
        ret = snprintf(buf, *plen, "%s\n", pctx->voice);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_STATUS)) {
        if (pctx->child1 == -1)
            ret = snprintf(buf, *plen, "IDLE\n");
        else
            ret = snprintf(buf, *plen, "BUSY\n");
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDSET) && (rscid == RSC_VOICE)) {
        strncpy(pctx->voice, val, VOICELEN-1);
        pctx->voice[VOICELEN - 1] = (char) 0;
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDSET) && (rscid == RSC_SPEAK)) {
        // Return an error if the tts is already in use
        if (pctx->child1 != -1) {
            ret = snprintf(buf, *plen, "BUSY\n");
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        // Create the pipe to listen for completion
        ret = pipe2(pctx->pipefd, O_NONBLOCK | O_CLOEXEC);
        if (ret != 0) {
            (void) snprintf(tmpbuf, MX_LINE, "pipe() call fails : %s", strerror(ret));
            edlog(tmpbuf);
            *plen = 0;     // Unusual error so logged but no UI error msg
            return;
        }
        // Add read fd to select list.  We get the speak status through it
        add_fd(pctx->pipefd[0], ED_READ, speak_complete, pctx);
        // spawn a process that will spawn and waitfor the flite command
        pctx->child1 = fork();
        if (pctx->child1 < 0) {
            (void) snprintf(tmpbuf, MX_LINE, "fork() call fails : %s", strerror(ret));
            edlog(tmpbuf);
            *plen = 0;     // Unusual error so logged but no UI error msg
            return;
        }
        if (pctx->child1 != 0) {
            // we are the parent. Return as there is nothing to output
           *plen = 0;
           return;
        }
        // We are the child of the first fork.  We fork and exec the flite command
        // and waitfor that command to finish.
        child2 = fork();
        if (child2 < 0) {
            (void) snprintf(tmpbuf, MX_LINE, "fork() call fails : %s", strerror(ret));
            edlog(tmpbuf);
            *plen = 0;     // Unusual error so logged but no UI error msg
            return;
        }
        if (child2 != 0) {
            // we are child1 and have forked the process that will exec the flite
            // command.  We wait for the second child to die, then we write the
            // return status down our end of the pipe and then we die.
            ret = (int) waitpid(child2, &wstatus, 0);
            if (ret == -1) {
                // error waiting for the flite command exit
                (void) snprintf(tmpbuf, MX_LINE, "fork() call fails : %s", strerror(ret));
                edlog(tmpbuf);
                *plen = 0;     // Unusual error so logged but no UI error msg
                return;
            }
            // write the return status as a string and send down the pipe
            ret = snprintf(tmpbuf, MX_LINE-1, "%d", wstatus);
            if (ret > 0) {
                tmpbuf[ret] = (char) 0;
                (void) write(pctx->pipefd[1], tmpbuf, (ret + 1));
            }
            // All done, exit
            exit(0);
        }

        // We are child2 and have been forked but not exec'd yet so we have a copy of the
        // original program context.  
        ret =  execl("/usr/bin/flite", "/usr/bin/flite", "-voice", pctx->voice,
                     val, (char  *) NULL);

        // to get here the above exec must have failed.  Log this error
        (void) snprintf(tmpbuf, MX_LINE, "execl() call fails : %s", strerror(ret));
        edlog(tmpbuf);
        // tell the parent we are done
        ret = snprintf(tmpbuf, MX_LINE-1, "%d", wstatus);
        if (ret > 0) {
            tmpbuf[ret] = (char) 0;
            (void) write(pctx->pipefd[1], tmpbuf, (ret + 1));
        }
        exit(0);
    }
    return;
}


/**************************************************************
 * speak_complete():  - Data on read end of pipe.  It is the status
 * of the now complete flite command.
 **************************************************************/
static void speak_complete(
    int      fd_in,         // FD with data to read,
    TTS     *pctx)          // our local info
{
    int      ret;                  // return count
    char     tmpbuf[MX_LINE];      // utility string
    int      wstatus;       // return status of the first fork

    ret = read(fd_in, tmpbuf, MX_LINE-1);

    if (ret > 0) {
        tmpbuf[ret] = (char) 0;
    }
    ret = (int) waitpid(pctx->child1, &wstatus, 0);

    // close the pipe fds
    del_fd(pctx->pipefd[0]);
    (void) close(pctx->pipefd[0]);
    (void) close(pctx->pipefd[1]);

    // tell the system that we are no longer busy
    pctx->child1 = -1;
}

// end of tts.c
