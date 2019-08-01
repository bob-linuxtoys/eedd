/*
 *  Name: irccom.c
 *
 *  Description: Simple interface to a Linux irccom device
 *
 *  Resources:
 *    config - IRC server name or IP and the robot's name 
 *    status - NoServer, Connecting, Connected, or Error
 *    available_channels - list of channels on server
 *    my_channels - which channels we substribe to
 *    comm - message from channels, messages to channels
 */

/*
 * Copyright:   Copyright (C) 2019 by Demand Peripherals, Inc.
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
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "../include/eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // resource names and numbers
#define FN_CONFIG          "config"
#define FN_STATUS          "status"
#define FN_AVCHAN          "available_channels"
#define FN_MYCHAN          "my_channels"
#define FN_COMM            "comm"
#define RSC_CONFIG         0
#define RSC_STATUS         1
#define RSC_AVCHAN         2
#define RSC_MYCHAN         3
#define RSC_COMM           4
        // What we are is a ...
#define PLUGIN_NAME        "irccom"
        // Maximum size of an IRC message (+1 for null)
#define IRC_MSGLEN         513
        // Maximum size of an IRC channel name (+1 for null)
#define IRC_CHNLEN         201
        // Maximum size of an IRC user nick name (+1 for null)
#define IRC_NCKLEN         10
        // maximum size of a line to the user on the comm resource
#define MX_LINE            (IRC_MSGLEN + IRC_CHNLEN + IRC_NCKLEN)
        // We limit the number of channels more to keep the
        // user interface sane.  Too many would be confusing.
#define NCHAN              2
        // Maximum length of the server IP address or domain name
#define SRVLEN             100
        // State of the link to the IRC server
#define ICM_NOSERVER       0
#define ICM_CONNECTING     1
#define ICM_CONNECTED      2
#define ICM_ERROR          3
        // Number of millisecond for retry timer
#define ICM_RETRY          10000
        // We retrieve the channel list after connecting. The
        // channel list has three states.
#define AVC_NOSERVER       0
#define AVC_RETRIEVING     1
#define AVC_AVAILABLE      2
        // IRC has different types of channels indicated by the first
        // character of the channel name.  A '#' indicates a channel
        // visible to the whole internet, and a '&' is a local channel.
        // Choose which you want here
#define AVC_TYPE           "&"


/**************************************************************
 *  - Data structures
 **************************************************************/
    // Info kept for each channel we subscribe to
typedef struct
{
    char     chname[IRC_CHNLEN]; // channel name
} CHINFO;

    // All state info for an instance of an irccom
typedef struct
{
    void    *pslot;             // handle to plug-in's's slot info
    int      status;            // connected, error, connecting, noserver
    void    *ptimer;            // timer with callback to timeout for connecting
    char     nam[IRC_NCKLEN];   // (nick)name for user
    char     srv[SRVLEN];       // the IRC server to use
    int      ircfd;             // FD to the IRC server
    char     inbuf[MX_LINE];    // Buffer of data from the IRC server
    int      inidx;             // Put next char into inbuf at this location
    char     avch[MXRPLY];      // available channel list
    int      avidx;             // location of next char to store 
    int      avstatus;          // not connected, retrieving, available
    CHINFO   chan[NCHAN];       // subscribed channel names
} IRCCOM;


/**************************************************************
 *  - Function prototypes and external references
 **************************************************************/
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void irc_receive(int fd, IRCCOM  *pctx);
static void irc_connect(void *timer, IRCCOM  *pctx);
static void finish_connect(int fd, IRCCOM  *pctx);
static int irc_command(IRCCOM  *, char *, int);
static void irc_line(char *line, int len, IRCCOM *pctx);
extern int DebugMode;


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    IRCCOM  *pctx;     // our local device context
    int      i;        // loop counter

    // Allocate memory for this plug-in
    pctx = (IRCCOM *) malloc(sizeof(IRCCOM));
    if (pctx == (IRCCOM *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in irccom initialization");
        return (-1);
    }

    // Init our IRCCOM structure
    pctx->pslot = pslot;       // this instance of the irccom 
    pctx->status = ICM_NOSERVER; // connected, error, connecting, noserver
    pctx->ptimer = (void *) 0; // no reconnect/timeout timers to start
    pctx->nam[0] = (char) 0;   // no nickname at start
    pctx->srv[0] = (char) 0;   // no IRC server at start
    pctx->ircfd = -1;          // no FD to server yet
    pctx->inidx = 0;           // no bytes in irccom receive buffer yet
    for (i = 0; i < NCHAN; i++) {   // no channels yet
        pctx->chan[i].chname[0] = (char) 0;
    }
    pctx->avidx =0;             // location of next char to store 
    pctx->avstatus = AVC_NOSERVER;   // not connected, retrieving, available

    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->priv = pctx;
    pslot->desc = "IRC communications";
    pslot->help = README;

    // Add handlers for the user visible resources
    pslot->rsc[RSC_CONFIG].name = FN_CONFIG;
    pslot->rsc[RSC_CONFIG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CONFIG].bkey = 0;
    pslot->rsc[RSC_CONFIG].pgscb = usercmd;
    pslot->rsc[RSC_CONFIG].uilock = -1;
    pslot->rsc[RSC_CONFIG].slot = pslot;
    pslot->rsc[RSC_STATUS].slot = pslot;
    pslot->rsc[RSC_STATUS].name = FN_STATUS;
    pslot->rsc[RSC_STATUS].flags = IS_READABLE;
    pslot->rsc[RSC_STATUS].bkey = 0;
    pslot->rsc[RSC_STATUS].pgscb = usercmd;
    pslot->rsc[RSC_STATUS].uilock = -1;
    pslot->rsc[RSC_AVCHAN].name = FN_AVCHAN;
    pslot->rsc[RSC_AVCHAN].flags = IS_READABLE;
    pslot->rsc[RSC_AVCHAN].bkey = 0;
    pslot->rsc[RSC_AVCHAN].pgscb = usercmd;
    pslot->rsc[RSC_AVCHAN].uilock = -1;
    pslot->rsc[RSC_AVCHAN].slot = pslot;
    pslot->rsc[RSC_MYCHAN].name = FN_MYCHAN;
    pslot->rsc[RSC_MYCHAN].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_MYCHAN].bkey = 0;
    pslot->rsc[RSC_MYCHAN].pgscb = usercmd;
    pslot->rsc[RSC_MYCHAN].uilock = -1;
    pslot->rsc[RSC_MYCHAN].slot = pslot;
    pslot->rsc[RSC_COMM].name = FN_COMM;
    pslot->rsc[RSC_COMM].flags = CAN_BROADCAST | IS_WRITABLE;
    pslot->rsc[RSC_COMM].bkey = 0;
    pslot->rsc[RSC_COMM].pgscb = usercmd;
    pslot->rsc[RSC_COMM].uilock = -1;
    pslot->rsc[RSC_COMM].slot = pslot;

    return (0);
}


/**************************************************************
 * usercmd():  - The user is reading or setting one of the configurable
 * resources. 
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
    IRCCOM  *pctx;     // our local info
    int      mxlen;    // maximum length of string to user
    int      ret;      // return count
    char    *ptmp;     // local copy of the *buf pointer on input
    char    *strp;     // used to step through words in the input line
    int      i;        // walk the list of channels
    char     tmpbuf[MX_LINE];      // utility string
    int      tmplen;               // length of tmpbuf
    int      err = 0;  // ==1 on irc_command errors


    pctx = (IRCCOM *) pslot->priv;

    if ((cmd == EDGET) && (rscid == RSC_CONFIG)) {
        ret = snprintf(buf, *plen, "%s %s\n", pctx->nam, pctx->srv);
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_AVCHAN)) {
        if (pctx->avstatus == AVC_NOSERVER)
            ret = snprintf(buf, *plen, "Unavailable, not connected\n");
        else if (pctx->avstatus == AVC_RETRIEVING)
            ret = snprintf(buf, *plen, "Unavailable, retrieving now\n");
        else {
            (void) strncpy(buf, pctx->avch, *plen);
            ret = pctx->avidx;
        }
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_STATUS)) {
        if (pctx->status == ICM_NOSERVER)
            ret = snprintf(buf, *plen, "No server\n");
        else if (pctx->status == ICM_CONNECTING)
            ret = snprintf(buf, *plen, "Connecting\n");
        else if (pctx->status == ICM_CONNECTED)
            ret = snprintf(buf, *plen, "Connected\n");
        else if (pctx->status == ICM_ERROR)
            ret = snprintf(buf, *plen, "Error\n");
        *plen = ret;  // (errors are handled in calling routine)
    }
    else if ((cmd == EDGET) && (rscid == RSC_MYCHAN)) {
        mxlen = *plen;       // on input plen is size of buffer
        *plen = 0;           // no character in (output) buffer to start
        for (i = 0; i < NCHAN; i++) {
            if (pctx->chan[i].chname[0] == (char) 0)  // only for valid names
                continue;
            ret = snprintf(&(buf[*plen]), (mxlen - *plen), "%s ", pctx->chan[i].chname);
            *plen += ret;
        }
        ret = snprintf(&(buf[*plen]), (mxlen - *plen), "\n");
        *plen += ret;
    }
    else if ((cmd == EDSET) && (rscid == RSC_CONFIG)) {
        pctx->status = ICM_CONNECTING;
        // Parse out the server and user nickname.  
        ptmp = val;      // get the original location of input string
        strp = strsep(&ptmp, " ");
        if (strp == NULL) {
            ret = snprintf(ptmp, *plen, E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        strncpy(pctx->nam, val, IRC_NCKLEN-1);
        pctx->nam[IRC_NCKLEN-1] = (char) 0;
        strncpy(pctx->srv, ptmp, SRVLEN-1);
        pctx->srv[SRVLEN-1] = (char) 0;

        pctx->status = ICM_CONNECTING;
        irc_connect(pctx->ptimer, pctx);

        pctx->avstatus = AVC_RETRIEVING;
        pctx->avidx = 0;

        *plen = 0;
    }
    else if ((cmd == EDSET) && (rscid == RSC_MYCHAN)) {
        // The user wants to connect to some new channels.  Stop listening
        // on the previous channels.
        if (pctx->status == ICM_CONNECTED) {
            for (i = 0; i < NCHAN; i++) {
                if (pctx->chan[i].chname[0] == (char) 0)  // null name?
                    continue;
                tmplen = snprintf(tmpbuf, MX_LINE, "PART %s%s\r\n", AVC_TYPE, pctx->chan[i].chname);
                err |= irc_command(pctx, tmpbuf, tmplen);
            }
        }
    
        // Parse out the requested channel names.  Note that we do not have 
        // the user add '&' as a prefix.  This lets the command work more
        // easily with shell commands.
        ptmp = val;
        for (i = 0; i < NCHAN; i++) {
            strp = strsep(&ptmp, " ");
            if (strp == NULL) {
                ret = snprintf(ptmp, *plen, E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            strncpy(pctx->chan[i].chname, strp, IRC_CHNLEN-1);
            pctx->chan[i].chname[IRC_CHNLEN-1] = (char) 0; // not needed
            if (pctx->status == ICM_CONNECTED) {
                if (pctx->chan[i].chname[0] == (char) 0)  // null name?
                    continue;
                tmplen = snprintf(tmpbuf, MX_LINE, "JOIN %s%s\r\n", AVC_TYPE, pctx->chan[i].chname);
                err |= irc_command(pctx, tmpbuf, tmplen);
            }
        }
    }
    else if ((cmd == EDSET) && (rscid == RSC_COMM)) {
        // Sanity checks for conected and valid channel
        if (pctx->status != ICM_CONNECTED) {
            ret = snprintf(buf, *plen, "Not connected\n");
            *plen = ret;
            return;
        }
        ptmp = val;
        strp = strsep(&ptmp, " ");
        if (strp == NULL) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        // strp now points to the null terminated channel to use
        // Verify that it is one of the channels in our list
        for (i = 0; i < NCHAN; i++) {
            if (strncmp(strp, pctx->chan[i].chname, IRC_CHNLEN) == 0)
                break;
        }
        if (i == NCHAN) {
            ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // Connected and first word is a valid channel. Send text.
        tmplen = snprintf(tmpbuf, MX_LINE, "PRIVMSG %s%s :%s\r\n",
                         AVC_TYPE, strp, ptmp);
        err |= irc_command(pctx, tmpbuf, tmplen);  // err=0 if no errors
        if (err != 0 ) {   // irc_command disconnects on errors
            ret = snprintf(buf, *plen, "Not connected\n");
            *plen = ret;
            return;
        }
    }
    return;
}


/**************************************************************
 * irc_connect():  - Try to connect to the IRC server.  Set a
 * timer on failure to try again.  This routine is called from
 * the user interface or via a timeout.  Either way we close
 * the existing socket, resolve the new host name, and do a
 * non-blocking connect() to the server.  A write callback is
 * registered to complete the connection when the connection
 * is established.
 **************************************************************/
static void irc_connect(
    void    *timer,    // handle of the timer that expired
    IRCCOM  *pctx)     // our local info
{
    int      ret=0;    // return count
    char     tmpbuf[MX_LINE];      // utility string
    struct addrinfo hints;         // used to get host address
    struct addrinfo *res;          // host info 
    struct addrinfo *resp;         // Used to loop over host list


    // We have a new config.  Close the existing connection and try
    // to open a new one to the IRC server
    if (pctx->ircfd >= 0) {
        del_fd(pctx->ircfd);
        close(pctx->ircfd); 
        pctx->ircfd = -1;
        pctx->inbuf[0] = (char) 0;
        pctx->inidx = 0;
        pctx->status = ICM_CONNECTING;
        pctx->avstatus = AVC_NOSERVER;
        pctx->avidx = 0;
    }
    // Remove the retry timer if it is set.  
    if (pctx->ptimer) {
        del_timer(pctx->ptimer);
        pctx->ptimer = 0;
    }

    // Give hints then try to resolve the server using getaddrinfo
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ret = getaddrinfo(pctx->srv, "6667", &hints, &res);
    if (ret != 0) {
        // log error message if in debug mode
        if (DebugMode) {
            (void) snprintf(tmpbuf, MX_LINE, "%s", gai_strerror(ret));
            edlog(tmpbuf);
        }
        pctx->ptimer = add_timer(ED_ONESHOT, ICM_RETRY, irc_connect, (void *) pctx);
        return;
    }
    // Resolved the server IP address(es).  Try to connect.
    for (resp = res; resp != NULL; resp = resp->ai_next) {
        pctx->ircfd  = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (pctx->ircfd == -1)
             continue;

        ret = connect(pctx->ircfd, resp->ai_addr, resp->ai_addrlen);
        // Non-blocking so we expect -1 and errno = EINPROGRESS
        if ((ret == -1) && (errno == EINPROGRESS)) {
            // register a write callback to complete the set up
            add_fd(pctx->ircfd, ED_WRITE, finish_connect, pctx);
            return;
        }
        // There was some problem, close and try the next host
        close(pctx->ircfd);
    }
 
    // Unable to resolve the address.  Set a timer to try again
    pctx->ptimer = add_timer(ED_ONESHOT, ICM_RETRY, irc_connect, (void *) pctx);
    freeaddrinfo(res);
    return;
}


/**************************************************************
 * finish_connect():  - This routine is a write callback that
 * is called when a connection to a server has been made and we
 * can now write to that connection.
 * Set the nickname and join the channels.
 **************************************************************/
static void finish_connect(
    int      fd,       // fd that generated this callbackd
    IRCCOM  *pctx)     // our local info
{
    int      ret=0;    // return count
    int      sockerr;  // set if the socket is not usable
    int      sizerr;   // sizeof sockerr
    int      i;        // walk the list of channels
    char     tmpbuf[MX_LINE];      // utility string
    int      tmplen;               // length of tmpbuf
    int      err = 0;  // ==1 on irc_command errors

    // Validate that the socket is really working.
    sizerr = sizeof(sockerr);
    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, (socklen_t *)&sizerr);
    if ((pctx->ircfd < 0) || (ret < 0) || (sockerr != 0)) {
        // Something went wrong.  Close the fd and try again
        del_fd(pctx->ircfd);
        close(pctx->ircfd); 
        pctx->ircfd = -1;
    }

    // We now have a TCP connection to the server.  Yeah!

    // Delete the fd from the select WRITE list
    del_fd(pctx->ircfd);

    // There should be a connection retry timer running. Cancel it.
    if (pctx->ptimer) {
        del_timer(pctx->ptimer);
        pctx->ptimer = 0;
    }

    // Login.  Set our name.
    tmplen = snprintf(tmpbuf, MX_LINE, "NICK %s\r\n", pctx->nam);
    err |= irc_command(pctx, tmpbuf, tmplen);  // err=0 if no errors
    tmplen = snprintf(tmpbuf, MX_LINE, "USER %s localhost %s :%s\r\n",
                      pctx->nam, pctx->srv, pctx->nam);

    err |= irc_command(pctx, tmpbuf, tmplen);  // err=0 if no errors

    // Tell the server what channels we want to hear
    for (i = 0; i < NCHAN; i++) {
        tmplen = snprintf(tmpbuf, MX_LINE, "JOIN %s%s\r\n", AVC_TYPE, pctx->chan[i].chname);
        err |= irc_command(pctx, tmpbuf, tmplen);
    }

    // Request a list of available channels
    tmplen = snprintf(tmpbuf, MX_LINE, "LIST\r\n");
    err |= irc_command(pctx, tmpbuf, tmplen);

    // Add the ircfd to our list of read fds if no errors.  The irc_command
    // routine will take care of cleaning up a failed connection.
    if (err == 0) {
        add_fd(pctx->ircfd, ED_READ, irc_receive, pctx);
        pctx->status = ICM_CONNECTED;
    }
}


/**************************************************************
 * irc_command():  - Send a command to the IRC server.  Return
 * 0 on success.  On error, delete the fd from the select loop,
 * close the fd, and start a timer for a reconnect.
 **************************************************************/
static int irc_command(
    IRCCOM  *pctx,     // this instance of irccom
    char    *sndbuf,   // buffer of characters to send
    int      sndlen)   // number of characters in the buffer
{
    char     tmpbuf[MX_LINE];      // utility string
    int      ret;      // system call return value

    if ((sndlen <= 0) || (pctx->ircfd < 0)) {
        return(1);     // Bogus string to send or not connected
    }

    while (1) {
        ret = write(pctx->ircfd, sndbuf, sndlen);
        if (ret == sndlen) {
            return(0);   // success return
        }

        // problem sending to server??
        // retry write on error EAGAIN
        if ((ret == -1) && (errno == EAGAIN)) {
            continue;    // try again (busy-wait but usually quick)
        }
        // log error if in debug mode
        if (DebugMode) {
            if (ret < 0) {
                (void) snprintf(tmpbuf, MX_LINE, "%s", strerror(errno));
                edlog(tmpbuf);
            }
            // else must be a partial write.  A partial write usually
            // indicates a full socket buffer and a communication problem.
            // Not strictly needed but we error out and retry the conn
            edlog("Partial write in IRCCOM.  Retrying connection");
        }

        // Getting here means there was a error.  Shutdown and start a
        // timer to retry the connection
        del_fd(pctx->ircfd);
        close(pctx->ircfd);
        if (pctx->ptimer)
            del_timer(pctx->ptimer);
        pctx->ptimer = add_timer(ED_ONESHOT, ICM_RETRY, irc_connect, (void *) pctx);
        pctx->status = ICM_CONNECTING;
        pctx->avstatus = AVC_NOSERVER;
        pctx->avidx = 0;
        return(1);    // error return
    }
}


/**************************************************************
 * irc_receive():  - Read data from the IRC server
 **************************************************************/
static void irc_receive(
    int      fd_in,         // FD with data to read,
    IRCCOM  *pctx)          // our local info
{
    int      ret=0;    // return count
    int      i;

    ret = read(pctx->ircfd, &(pctx->inbuf[pctx->inidx]), (MX_LINE - pctx->inidx));
    if (ret > 0) {
        // Did a read and have new characters in the buffer.  Look
        // for new lines and do any needed processing on them.
        pctx->inidx += ret;
        while (1) {
            for (i = 0; i < pctx->inidx; i++) {
                // accept either CR of LF as line terminator
                if ((pctx->inbuf[i] == '\r') || (pctx->inbuf[i] == '\n'))
                    break;
            }
            if (i == pctx->inidx) {  // scan found no terminators?
                return;              // no more complete lines in the buffer.
            }
            // terminator at location i
            pctx->inbuf[i] = (char) 0;   // replace terminator with null

            // We have a line from the IRC server.  Send to IRC processor
            // that will consume the line.
            irc_line(pctx->inbuf, i, pctx);

            // Got a line and processed it.
            // Move any remaining characters in the buffer down and
            // continue looking for complete lines.
            if (pctx->inidx - (i+1) != 0) {
                memmove(pctx->inbuf, &(pctx->inbuf[i+1]), (pctx->inidx - (i+1)));
            }
            pctx->inidx -= i+1;    // adjust buffer index for removed chars
        }
    }
    if ((ret < 0) && (errno == EAGAIN)) {
        return;    // return and select will bring us back
    }

    // close (ret=0) or non-recoverable error (rec<0).  Restart conn
    del_fd(pctx->ircfd);
    close(pctx->ircfd);
    pctx->ircfd = -1;
    if (pctx->ptimer) {   // delete existing timer if one
        del_timer(pctx->ptimer);
        pctx->ptimer = 0;
    }
    pctx->ptimer = add_timer(ED_ONESHOT, ICM_RETRY, irc_connect, (void *) pctx);
    pctx->status = ICM_CONNECTING;
    pctx->avstatus = AVC_NOSERVER;
    pctx->avidx = 0;
    return;
}


/**************************************************************
 * irc_line():  - Process a line of text from the server
 **************************************************************/
static void irc_line(char *line, int len, IRCCOM  *pctx)
{
    SLOT      *pslot;   // This instance of the hellodemo plug-in
    RSC       *prsc;    // pointer to this slot's counts resource
    char      *ptr;     // points into the line
    int        numcmd;  // numeric command 
    int        msglen;  // length of string to send to user
    char       lnout[MX_LINE]; // line of text to the user
    char      *strp;    // help parse the line
    int        ret;

    ptr = line;

    // A single CR or LF will be treated as a line of length 0
    if (len == 0) {
        return;
    }
    if (line[0] == ':') {
        (void) strsep(&ptr, " \t");
        if ( ! ptr)
            return;
    }
    // past the optional prefix.  Next up is the "command" which can be
    // either numeric or a string
    if (1 == sscanf(ptr, "%d", &numcmd)) {
        // Got a numeric command.
        if (numcmd == 323) {      // "End of LIST"
            // Go from retrieving list to list available
            pctx->avstatus = AVC_AVAILABLE;
        }
        else if (numcmd == 322) {  // Part of channel list response
            (void) strsep(&ptr, AVC_TYPE);    // get to the channel name
            if ( ! ptr) return;
            strp = strsep(&ptr, " ");         // get past the name
            if ( ! ptr) return;
            (void) strsep(&ptr, ":");         // get to the channel topic
            if ( ! ptr) return;
            // add channel name and topic to available_channels list
            ret = snprintf(&(pctx->avch[pctx->avidx]), (MXRPLY - pctx->avidx), "%s %s\n",
                          strp, ptr);
            if (ret <= 0) return;
            pctx->avidx += ret;
        }
        return;
    }
    // Must be string command.  Do strcmp() to find which command it is
    if ( ! strncmp("PING", ptr, 4)) {
        // Echo line back replacing PING with PONG
        ptr[1] = 'O';
        write(pctx->ircfd, ptr, strlen(ptr));
        return;
    }
    else if ( ! strncmp("PONG ", ptr, 4)) {
        // Ignore responses to our PING requests
        return;
    }
    else if ( ! strncmp("PRIVMSG", ptr, 7)) {
        // "PRIVMSG &redteam :body of message here
        // Text for the user.  Remove everything up to the channel type
        strp = strsep(&ptr, AVC_TYPE);
        if (*ptr == (char) 0) {      // ptr will be non-NULL if we found the type
            return;
        }
        // ptr points to the channel name without the AVC_TYPE character
        // move ptr to ':' and remove it
        strp = strsep(&ptr, ":");
        if (*ptr == (char) 0) {      // ptr will be non-NULL if we found the ":"
            return;
        }

        // strp points to the first character of the channel name
        // ptr points to the first character of the message body
        pslot = pctx->pslot;
        prsc = &(pslot->rsc[RSC_COMM]);  // message resource
        // Broadcast it if any UI are monitoring it.
        if (prsc->bkey == 0) {
            return;
        }
        // bkey will return cleared if UIs are no longer monitoring us
        // replace the null with a newline and send one more than the
        // string length
        msglen = snprintf(lnout, MX_LINE, "%s%s\n", strp, ptr);
        bcst_ui(lnout, msglen, &(prsc->bkey));
    }
    return;
}


// end of irccom.c
