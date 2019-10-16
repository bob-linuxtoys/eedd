/*
 * Name: ui.c
 *
 * Description: This file contains code to handle the protocol and 
 *              connections from users of the empty daemon.
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


#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <syslog.h>    /* for log levels */
#include <netinet/in.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>    /* for 'offsetof' */
#include <arpa/inet.h> /* for inet_addr() */
#include <dlfcn.h>
#include "main.h"


/***************************************************************************
 *  - Variable allocation and initialization
 ***************************************************************************/
int      nui = 0;              // number of open UI connections
int      srvfd;                // FD to the listening socket
char     prmpchar[] = { PROMPT, 0 };


/***************************************************************************
 *  - Function prototypes, forward references, and externs
 ***************************************************************************/
void            open_ui_port();
int             add_so(char *);
void            initslot(SLOT *);  // Load and init this slot
static void     open_ui_conn(int srvfd, int cb_data);
static void     close_ui_conn(int cn);
static void     receive_ui(int, int);
extern SLOT     Slots[];       // table of plug-in info
extern UI       UiCons[MX_UI]; // table of UI connections
extern int      Verbosity;     // verbosity level
extern int      UiaddrAny;     // Use any IP address if set
extern int      UiPort;        // TCP port for ui connections


/***************************************************************************
 * parse_and_execute(): - This routine parses the null terminated
 * command line found in cmd[] character array in the passed UI pointer.
 * Result are passed to UI fd.  A write error can cause the closure
 * of the UI fd and the freeing of the UI structure.
 *
 * Input:        Pointer to UI structure with new command.
 * Output:       void
 * Effects:      the internal state of the plug-in specified
 ***************************************************************************/
void parse_and_execute(UI *pui)
{
    char    *ccmd;       // command to be executed as a string
    int      icmd;       // command to be executed as an int
    char    *cslot;      // slot ID as a string (or name)
    int      islot;      // slot ID as an int
    char    *crsc;       // target resource in plug-in in slot
    int      irsc;       // target resource as an integer
    char    *val;        // value to be written if a write cmd
    char    *saveptr;    // lets us use a thread-safe strtok
    int      err;        // return code
    int      len;        // a string length
    int      bkey;       // broadcast key = slot/rsc
    RSC     *prsc;       // a plug-in's resource table or a single rsc
    char     rply[MXRPLY]; // reply back to the UI on error
    int      i;          // generic loop counter


    if ((pui->cmd == 0) || (pui->cmd[0] == 0) || (pui->cmdindx >= MXCMD) ||
        (pui->cmd[0] == '\n') || (pui->cmd[0] == '\r')) {
        return;   // nothing to do or an error
    }

    // Show/log commands if really verbose
    if (Verbosity >= ED_VERB_WARN) {
        for (i = 0; i < pui->cmdindx; i++) {   // replace \n\r with null
            if ((pui->cmd[i] == '\n') || (pui->cmd[i] == '\r')) {
                pui->cmd[i] = (char) 0;
                break;
            }
        }
        edlog("COMMAND : %s", pui->cmd);
    }

    /* Tokenize the input line */
    ccmd  = strtok_r(pui->cmd, " \t\n\r", &saveptr);

    // Get the command. 
    if (!strcmp(ccmd, CPREFIX "set"))
        icmd = EDSET;
    else if (!strcmp(ccmd, CPREFIX "get"))
        icmd = EDGET;
    else if (!strcmp(ccmd, CPREFIX "cat"))
        icmd = EDCAT;
    else if (!strcmp(ccmd, CPREFIX "list"))
        icmd = EDLIST;
    else if (!strcmp(ccmd, CPREFIX "loadso"))
        icmd = EDLOAD;
    else {
        // Report bogus command
        len = snprintf(rply, MXRPLY, E_BDCMD, ccmd);
        send_ui(rply, len, pui->cn); 
        return;
    }

    /* Do list command */
    if (icmd == EDLIST) {
        cslot  = strtok_r(NULL, " \t\r\n", &saveptr);
        // Print list of all plug-in if there was no second argument
        // Give description of plug-in if one was specified
        if (cslot == 0) {
            // edlist without argument -- list all plug-in
            len = snprintf(rply, MXRPLY, "  Slot/Name         Description\n");
            send_ui(rply, len, pui->cn);
            for (islot = 0; islot < MX_PLUGIN; islot++) {
                if ((Slots[islot].name != 0) &&
                    (Slots[islot].desc != 0)) {
                    len = snprintf(rply, MXRPLY, LISTFORMAT, islot,
                        Slots[islot].name, Slots[islot].desc);
                    send_ui(rply, len, pui->cn);
                    // sent the board and description. Now send the resources
                    for (irsc = 0; irsc < MX_RSC; irsc++) {
                       prsc = &(Slots[islot].rsc[irsc]);
                       if (prsc->name != 0) {
                           len = snprintf(rply, MXRPLY, LISTRSCFMT, prsc->name,
                               ((prsc->flags & IS_READABLE) ? CPREFIX "get " : ""),
                               ((prsc->flags & IS_WRITABLE) ? CPREFIX "set " : ""),
                               ((prsc->flags & CAN_BROADCAST) ? CPREFIX "cat " : ""));
                           send_ui(rply, len, pui->cn);
                       }
                    }
                }
            }
            prompt(pui->cn);
            return;
        }
        // Second argument to edlist is a plug-in name.  Find it.
        for (islot = 0; islot < MX_PLUGIN; islot++) {
            if ((Slots[islot].name != 0) &&
                (!strcmp(Slots[islot].name, cslot)))
                break;  //got it
        }
        if ((islot != MX_PLUGIN) && (Slots[islot].help != 0)) {
            len = strlen(Slots[islot].help);
            send_ui(Slots[islot].help, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
        len = snprintf(rply, MXRPLY, "Plug-in '%s' is not the the system\n", cslot);
        send_ui(rply, len, pui->cn); 
        prompt(pui->cn);
        return;
    }

    /* Do loadso command */
    if (icmd == EDLOAD) {
        cslot  = strtok_r(NULL, " \t\r\n", &saveptr);  // get plug-in file name
        if (cslot == 0) {
            // edloadso without argument -- this is an error
            len = snprintf(rply, MXRPLY, M_BADSLOT, "(null)");
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
        // Second argument to loadso is a plug-in name.  Try to load it
        islot  = add_so(cslot);
        if (islot >= 0) {
            initslot(&(Slots[islot]));   // run the initializer for the slot
        }
        prompt(pui->cn);
        return;
    }

    // Parse rest of line.
    cslot = strtok_r(NULL, " \t\r\n", &saveptr);
    crsc  = strtok_r(NULL, " \t\r\n", &saveptr);
    val   = strtok_r(NULL, "\r\n", &saveptr);

    /* get and validate slot or plug-in name */
    if ((cslot == NULL) || (strlen(cslot) == 0)) {
        // Report bogus board ID
        len = snprintf(rply, MXRPLY, E_NOPERI, "(null)");
        send_ui(rply, len, pui->cn); 
        prompt(pui->cn);
        return;
    }
    /* if the first character of slot is numeric, get slot ID */
    if (isdigit(cslot[0])) {
        err = sscanf(cslot, "%d", &islot);
        if ((err != 1) || (islot < 0) || (islot >= MX_PLUGIN)) {
            // Report bogus slotid ID
            len = snprintf(rply, MXRPLY, E_BDSLOT, cslot);
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
    }
    else {     /* not a digit, search for peri by name */
        len = strlen(cslot);
        for (islot = 0; islot < MX_PLUGIN; islot++) {
            if (Slots[islot].name == NULL)   // skip over empty slots
                continue;
            if (!strncmp(Slots[islot].name, cslot, len))
                break;  //got it
        }
        if (islot == MX_PLUGIN) {
            // Report no slot called cslot in board
            len = snprintf(rply, MXRPLY, E_NOPERI, cslot);
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
    }

    /* Got the slot ID.  Now validate and get the resource index */
    if ((crsc == NULL) || (strlen(crsc) == 0)) {
        // report an empty/invalid resource was specified
        len = snprintf(rply, MXRPLY, E_NORSC, "(null)", Slots[islot].name);
        send_ui(rply, len, pui->cn); 
        prompt(pui->cn);
        return;
    }
    prsc = Slots[islot].rsc;
    for (irsc = 0; irsc < MX_RSC; irsc++) {
        if ((prsc[irsc].name != 0) && (!strcmp(crsc, prsc[irsc].name)))
            break;  //got it
    }
    if (irsc == MX_RSC) {
        // report no resource rsc in board/slot
        len = snprintf(rply, MXRPLY, E_NORSC, crsc, Slots[islot].name);
        send_ui(rply, len, pui->cn); 
        prompt(pui->cn);
        return;
    }
    /* get pointer to resource */
    prsc = &(prsc[irsc]);   // get pointer to a single resource

    /* Got valid command, board, slot, and resource */
    /* Do per command error checking and processing */
    if (icmd == EDGET) {
        if ((prsc->flags & IS_READABLE) == 0) {
            // report that rsc is not readable
            len = snprintf(rply, MXRPLY, E_NREAD, crsc);
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
        if (prsc->uilock >= 0) {
            // report that another ui is already reading the rsc
            len = snprintf(rply, MXRPLY, E_BUSY, cslot);
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
        // All set.  Call the read routine.
        if (prsc->pgscb) {
            len = MXRPLY;
            (prsc->pgscb)(icmd, irsc, val, &(Slots[islot]), pui->cn, &len, rply);
            // Send response or error messages back to the user
            if ((len > 0) && (len < MXRPLY)) {
                send_ui(rply, len, pui->cn);
                prompt(pui->cn);
            }
        }
        // Done.  The plug-in will send the response back to the UI */
        return;
    }
    else if (icmd == EDSET) {
        if ((prsc->flags & IS_WRITABLE) == 0) {
            // report that rsc is not writable
            len = snprintf(rply, MXRPLY, E_NWRITE, crsc);
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
        /* Got board, slot, resource name or slot.  It's a set so now validate 'val' */
        if ((val == NULL) || (strlen(val) == 0)) {
            // report an empty/invalid value was specified
            len = snprintf(rply, MXRPLY, E_BDVAL, Slots[islot].rsc[irsc].name);
            send_ui(rply, len, pui->cn); 
            prompt(pui->cn);
            return;
        }
        // All set.  Call the write routine.
        if (prsc->pgscb) {
            len = MXRPLY;
            (prsc->pgscb)(icmd, irsc, val, &(Slots[islot]), pui->cn, &len, rply);
            // Send any error messages back to user
            if ((len > 0) && (len < MXRPLY)) {
                send_ui(rply, len, pui->cn);
            }
            prompt(pui->cn);
        }
        return;
    }
    else if (icmd == EDCAT) {
        // Set the broadcast key (bkey) in the resource.  If set the
        // slot's packet handler will search all the UI conns for a
        // matching key and send a copy of the sensor data down that
        // UI connection.  Here all we need to do is set the key in
        // the UI struct and in the resource's struct.
        // First a sanity check
        if ((prsc->flags & CAN_BROADCAST) == 0) {
            // report that rsc is not a broadcast sensor
            len = snprintf(rply, MXRPLY, E_NREAD, crsc);
            send_ui(rply, len, pui->cn);
            prompt(pui->cn);
            return;
        }
        // Record that this UI is monitoring and tell the resource
        bkey  = (islot & 0xff) << 16;   // bkey is slot/rsc
        bkey += (irsc  & 0xff);         // bkey is slot/rsc
        pui->bkey = bkey;       // mark UI in monitor mode
        prsc->bkey = bkey;      // tell resource that at least one UI is monitoring
        // Tell the resource that someone is listening.  This allows the resource
        // to configure itself or enable auto-updates from the plug-in.
        if (prsc->pgscb) {
            len = MXRPLY;
            (prsc->pgscb)(icmd, irsc, val, &(Slots[islot]), pui->cn, &len, rply);
        }
    }
    return;
}


/***************************************************************************
 * bcst_ui(): - Broadcast the buffer down all UI connections that
 * have a matching monitor key.  Clear the key if there are no UIs
 * monitoring this resource any more.  Close UI sessions that fail
 * the write.
 ***************************************************************************/
void bcst_ui(
    char    *buf,         // buffer of chars to send
    int      len,         // number of chars to send
    int     *bkey)        // slot/rsc as an int
{
    UI      *pui;         // pointer to UI connection
    int      cn;          // indes to above
    int      nwr;         // number of bytes written
    int      ret;         // write() return value
    int      newbkey;     // to clear bkey if no listeners

    /* Sanity checks */
    if ((len <= 0) || (*bkey == 0)) {
        // Nothing to do
        return;
    }

    // Walk all UI conns looking for matching bkey
    newbkey = 0;
    for (cn = 0, pui = UiCons; cn < MX_UI; cn++, pui++) {
        if ((pui->fd < 0) || (pui->bkey != *bkey))  {
            continue;
        }

        // Got an open ui conn that is catting this resource
        newbkey = *bkey;
        nwr = 0;
        while (nwr != len) {
            ret = write(pui->fd, buf, len);
            if (ret <= 0) {
                if (errno != EAGAIN)
                    continue;
                close_ui_conn(cn);
                break;
            }
            nwr += ret;
        }
    }

    // Reset the resources bkey (ie clear it or re-set it)
    *bkey = newbkey;

    return;
}


/***************************************************************************
 * send_ui(): - This routine is called to send data to the other
 * end of a UI connection.  Close the connection on error.
 *
 ***************************************************************************/
void send_ui(
    char    *buf,         // buffer of chars to send
    int      len,         // number of chars to send
    int      cn)          // index to UI conn table
{
    int      nwr;         /* number of bytes written */

    /* Sanity checks */
    if ((len < 0) || (cn < 0) || (cn >= MX_UI) || (UiCons[cn].fd < 0)) {
        return;   // nothing to do or bogus request
    }
    buf[len] = (char) 0;  // make it a null terminated string

    // Show/log commands if really verbose
    if (Verbosity >= ED_VERB_INFO) {
        edlog("RESPONSE: %s\n", buf);
    }

    while (len) {
        nwr = write(UiCons[cn].fd, buf, len);
        if (nwr <= 0) {
            if (errno != EAGAIN)
                continue;
            close_ui_conn(cn);
            return;
        }
        len -= nwr;
    }
    return;
}


/***************************************************************************
 * prompt(): -  Send a prompt character to the other
 * end of a UI connection.  Close the connection on error.
 * A prompt indicates the completion of the previous command.
 *
 ***************************************************************************/
void prompt(
    int      cn)          // index to UI conn table
{
    int      nwr=0;       // number of bytes written

    /* Sanity checks */
    if ((cn < 0) || (cn >= MX_UI) || (UiCons[cn].fd < 0)) {
        return;   // nothing to do or bogus request
    }

    while (nwr != 1) {
        nwr = write(UiCons[cn].fd, prmpchar, 1);
        if (nwr <= 0) {
            if (errno != EAGAIN)
                continue;
            close_ui_conn(cn);
            return;
        }
    }
    return;
}


/***************************************************************************
 * receive_ui(): - This routine is called to read data
 * from a TCP connection.  We look for an end-of-line and pass
 * full lines to the CLI parser.  
 *
 * Input:        FD of socket with data to read
 * Output:       void
 * Effects:      the Baseboard vie the CLI parser
 ***************************************************************************/
void receive_ui(int fd_in, int cb_data)
{
    int      nrd;            /* number of bytes read */
    int      i;              /* a temp int */
    int      gotline;        /* set true if we get a full line */
    int      cn;             /* index into UiCons */
    UI      *pui;            /* pointer to UI at cn */

    /* Locate the UI struct with fd equal to fd_in */
    for (cn = 0 ; cn < MX_UI; cn++) {
        if (fd_in == UiCons[cn].fd) {
            break;
        }
    }
    /* Error if we could not find the fd */
    if (MX_UI == cn) {
        /* Take this bogus fd out of the select loop */
        del_fd(fd_in);
        close(fd_in);
        return;
    }
    pui = &(UiCons[cn]);

    /* We read data from the connection into the buffer in the ui struct. Once
     * we've read all of the data we can, we scan for a newline character and
     * pass any full lines to the parser. */
    nrd = read(pui->fd, &(pui->cmd[pui->cmdindx]), (MXCMD - pui->cmdindx));

    /* shutdown manager conn on error or on zero bytes read */
    if ((nrd <= 0) && (errno != EAGAIN)) {
        close_ui_conn(cn);
        return;
    }

    pui->cmdindx += nrd;

    /* The commands are in the buffer. Call the parser to execute them */
    do {
        gotline = 0;
        // Scan for a newline.    If found, replace it with a null
        for (i = 0; i < pui->cmdindx; i++) {
            if (pui->cmd[i] == '\n') {
                pui->cmd[i] = (char) 0;
                gotline = 1;
                parse_and_execute(pui);
                (void) memmove(pui->cmd, &(pui->cmd[i+1]), (pui->cmdindx - (i+1)));
                pui->cmdindx -= i+1;
                break;
            }
        }
    } while ((gotline == 1) && (pui->cmdindx > 0));

    return;
}



/***************************************************************************
 * open_ui_conn(): - Accept a new UI manager conn.
 * This routine is called when a user interface program wants
 * read or write parameters to the Baseboard.
 * This routine is the read callback for the UI listen socket.
 *
 * Input:        The file descriptor of the UI server socket
 *               and callback data
 * Output:       void
 * Effects:      manager connection table (ui)
 ***************************************************************************/
void open_ui_conn(int srvfd, int cb_data)
{
    int      newuifd;    /* New UI FD */
    socklen_t adrlen;    /* length of an inet socket address */
    struct sockaddr_in cliskt; /* socket to the UI/DB client */
    int      flags;      /* helps set non-blocking IO */
    int      i;

    /* Accept the connection */
    adrlen = (socklen_t) sizeof(struct sockaddr_in);
    newuifd = accept(srvfd, (struct sockaddr *) &cliskt, &adrlen);
    if (newuifd < 0) {
        return;
    }

    /* We've accepted the connection.    Now get a UI structure. */
    for (i = 0; i < MX_UI; i++) {
        if (UiCons[i].fd == -1) {
            break;
        }
    }
    if ((i == MX_UI) && (nui >= MX_UI)) {
        /* Oops, out of UI conns.  Log it and close the new conn */
        edlog(M_NOUI);
        close(newuifd);
        return;
    }
    nui++;       /* increment number of UI structs alloc'ed */
    listen(srvfd, MX_UI - nui);  //  lower the number of avail conns

    /* OK, we've got the UI struct.  Fill it in.    */
    UiCons[i].fd = newuifd;
    flags = fcntl(UiCons[i].fd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void) fcntl(UiCons[i].fd, F_SETFL, flags);
    UiCons[i].o_ip = (int) cliskt.sin_addr.s_addr;
    UiCons[i].o_port = (int) ntohs(cliskt.sin_port);
    UiCons[i].cmdindx = 0;
    UiCons[i].bkey = 0;    // not watching inputs/sensors

    /* add the new UI conn to the read fd_set in the select loop */
    add_fd(newuifd, ED_READ, receive_ui, (void *) 0);

    return;
}



/***************************************************************************
 * close_ui_conn(): - Close an existing UI conn.
 *
 * Input:        Index of conn to close
 * Output:       void
 * Effects:      manager connection table (ui)
 ***************************************************************************/
void close_ui_conn(int cn)
{
    close(UiCons[cn].fd);
    del_fd(UiCons[cn].fd);
    UiCons[cn].fd = -1;
    nui--;
    listen(srvfd, MX_UI - nui);  //  lower the number of avail conns
    return;
}



/***************************************************************************
 * open_ui_port(): - Open the UI port for this application.
 *
 * Input:        int ui_port
 *               char *ui_addr;  the IP address to bind to
 * Output:       none
 * Effects:      select listen table
 ***************************************************************************/
void open_ui_port()
{
    struct sockaddr_in srvskt;
    int      adrlen;
    int      flags;

    adrlen = sizeof(struct sockaddr_in);
    (void) memset((void *) &srvskt, 0, (size_t) adrlen);
    srvskt.sin_family = AF_INET;
    srvskt.sin_addr.s_addr = (UiaddrAny) ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    srvskt.sin_port = htons(UiPort);
    if ((srvfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
        edlog(M_BADCONN, errno);
        return;
    }
    flags = fcntl(srvfd, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void) fcntl(srvfd, F_SETFL, flags);
    if (bind(srvfd, (struct sockaddr *) &srvskt, adrlen) < 0) {
        edlog(M_BADCONN, errno);
        return;
    }
    if (listen(srvfd, MX_UI - nui) < 0) {
        edlog(M_BADCONN, errno);
        return;
    }

    /* If we get to here, then we were able to open the UI socket. Tell the
     * select loop about it. */
    add_fd(srvfd, ED_READ, open_ui_conn, (void *) 0);
}

/***************************************************************************
 *  add_so()   - Put .so file name from cmd line into Slot.  Ignore request
 *  if no empty slots.  Returns -1 on error or the slot number on success.
 ***************************************************************************/
int add_so(
    char *so_name)   // Shared object file name to add
{
    int      i;         // loop counter
    int      len;       // length of plug-in shared object file name

    // Sanity check the length of the plug-in file name
    len = strnlen(so_name, MX_SONAME);
    if (len == MX_SONAME) {
        edlog(M_BADSLOT, so_name);
        return(-1);
    }
    for (i = 0; i < MX_PLUGIN; i++) {
        if (strnlen(Slots[i].soname, MX_SONAME) == 0) {
            strncpy(Slots[i].soname, so_name, MX_SONAME);
            return(i);
        }
    }
    // To get here means there were no empty slots.  Ignore request.
    edlog(M_NOSLOT, so_name);
    return(-1);
}


/***************************************************************************
 *  initslot()  - Load .so file and call init function for it
 ***************************************************************************/
void initslot(                          // Load and init this slot
    SLOT          *pslot)
{
    void          *handle;
    int          (*Initialize) (SLOT *);
    char           pluginpath[PATH_MAX]; // Has full name of the .so file
    int            i;
    int            k;  // used to build slot paths
    const char    *errmsg;

    // Ignore uninitialized slots
    if (pslot->soname[0] == (char) 0)
        return;

    k = sprintf(pluginpath, LIB_DIR);
    for (i = 0; i < strlen(pslot->soname); i++) {
        pluginpath[k++] = pslot->soname[i];
    }
    pluginpath[k++] = (char) 0;

    if (Verbosity) {
        printf("Adding plug-in '%s' to slot %d\n", pluginpath,
            pslot->slot_id);
    }

    // Try to open the .so file.
    dlerror();                  /* Clear any existing error */
    handle = dlopen(pluginpath, RTLD_NOW | RTLD_GLOBAL);
    pslot->handle = handle;
    if (handle == NULL) {
        edlog(M_BADSO, pluginpath);
        pslot->soname[0] = (char) 0;  // void this bogus plug-in entry
        return;
    }

    // get the runtime address of the Initialize function and call it.
    dlerror();                  /* Clear any existing error */
    *(void **) (&Initialize) = dlsym(handle, "Initialize");
    errmsg = dlerror();         /* correct way to check for errors */
    if (errmsg != NULL) {
        edlog(M_BADSYMB, "'Initialize'", pslot->soname);
        pslot->soname[0] = (char) 0;  // void this bogus plug-in entry
        return;
    }

    if (Initialize(pslot) < 0) {
        edlog(M_BADDRIVER, pslot->soname);
        pslot->soname[0] = (char) 0;  // void this bogus plug-in entry
        return;
    }
}


