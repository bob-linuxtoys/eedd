/*
 * Name: edcli.c
 *
 * Description: A utility that issues commands to the empty daemon using
 *              the command and parameters specified on the command line.
 *              This utility obliviates the need to 'telnet localhost 8870'
 *              to test plug-ins.
 *
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
 *
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <getopt.h>
#include <arpa/inet.h> /* for inet_addr() */
#include "main.h"



/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // Maximum length of an IP address string
#define MAX_IP       50
        // Max length of cmd down to the empty daemon
#define MAX_EDCMD   250


/**************************************************************
 *  - Function prototypes and forward references
 **************************************************************/
void usage();
void help(char **);
char usagetext[];
char helpget[];
char helpset[];
char helpcat[];
char helplist[];




/**************************************************************
 * edcli(): - Send commands to an empty daemon.  This program lets
 *           you control your application from the command line.
 *
 * Input:        argc, argv
 * Output:       0 on normal exit, -1 on error exit with errno set
 **************************************************************/
int main(int argc, char **argv)
{
    char bindaddress[MAX_IP]; // destination IP address as a string
    int  bindport;          // TCP port to the empty daemon
    int  cmdc;              // command line character option
    int  tmp_int;           // a temporary integer
    static int srvfd = -1;  // FD for empty daemon socket
    struct sockaddr_in skt; // network address for empty daemon
    int  adrlen;
    int  i;                 // generic loop counter
    int  ret;               // generic return value
    char buf[MAX_EDCMD];    // command to send to empty daemon
    int  slen;              // length of string in buf
    int  nout;              // num chars written to empty daemon
    char c;                 // response character from empty daemon

    // Set default config.  Change default behavior here.
    strncpy(bindaddress, "127.0.0.1", MAX_IP-1);
    bindport = 8870;


    // Do a sanity check.  Must be invoked as one of the ed commands
    if (strcmp(argv[0], "edget") &&
        strcmp(argv[0], "edset") &&
        strcmp(argv[0], "edcat") &&
        strcmp(argv[0], "edlist") &&
        strcmp(argv[0], "edloadso")) {
        // Unrecognized command
        printf("Unrecognized command '%s'.  Commands must be one of\n", argv[0]);
        printf(" edget, edset, edcat, edlist, or edloadso\n");
        exit(-1);
    }


    optind = 0;          // reset the scan of the cmd line arguments
    optarg = argv[0];
    while ((cmdc = getopt(argc, argv, "a:hp:")) != EOF) {
        switch ((char) cmdc) {
        case 'a':       // Bind Address
            strncpy(bindaddress, optarg, MAX_IP);
            break;

        case 'h':       // Help text
            help(argv);
            exit(0);
            break;

        case 'p':       // Bind Port
            if (sscanf(optarg, "%d", &tmp_int)) {
                bindport = tmp_int;
            }
            break;

        default:
            usage();
            exit(-1);
            break;
        }
    }

    // Open connection to empty daemon
    adrlen = sizeof(struct sockaddr_in);
    (void) memset((void *) &skt, 0, (size_t) adrlen);
    skt.sin_family = AF_INET;
    skt.sin_port = htons(bindport);
    if ((inet_aton(bindaddress, &(skt.sin_addr)) == 0) ||
        ((srvfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) ||
        (connect(srvfd, (struct sockaddr *) &skt, adrlen) < 0)) {
        printf("Error: unable to connect to the empty daemon.\n");
        exit(-1);
    }

    // At this point the connection to the empty daemon is open
    // We can now send it the command and arguments from the
    // command line.  Note that the command to send is actually
    // argv[0], that is, how this program was invoked.  Note
    // also that we start at optind, after all cmd parms.
    slen = sprintf(buf, "%s ", argv[0]);
    for (i = optind; (i < argc && slen < MAX_EDCMD -1) ; i++) {
        slen += snprintf(&(buf[slen]), (MAX_EDCMD - slen), "%s ", argv[i]);
    }
    if (slen >= MAX_EDCMD -1) {
        printf("Error: command exceeds maximum length of %d\n", MAX_EDCMD);
        exit(-1);
    }
    // replace trailing space of last param with a newline
    buf[slen-1] = '\n';

    // write complete command to the empty daemon
    nout = 0;
    while (nout != slen) {
        ret = write(srvfd, &(buf[nout]), (slen - nout));
        if ((ret < 0) && (errno == EAGAIN))
            continue;
        else if (ret <= 0) {
            printf("Error writing to empty daemon\n");
            exit(-1);
        }
        nout += ret;
    }

    // Command is sent, now print the response if any.
    // The empty daemon says the command is done by sending a ':'
    // character as a prompt.  Exit this program when we see
    // a prompt character.  This means that if invoked as a
    // edcat command you will need to use ^C to exit.
    while(1) {
        // character at a time so this won't be very efficient
        ret = read(srvfd, &c, 1);
        if ((c == PROMPT) || (ret == 0)) {
            exit(0);
        }
        else if ((ret < 0) && (errno != EAGAIN)) {
            printf("\nError reading from empty daemon connection\n");
            exit(-1);
        }
        if (errno == EAGAIN) {
            continue;
        }
        // Just a character to pass to the user
        putchar(c);
    }

    exit(0);
}


/**************************************************************
 * usage():  Print command syntax
 **************************************************************/
void usage()
{
    printf("%s", usagetext);

    return;
}


/**************************************************************
 * help():  Print useful help text based on invocation
 **************************************************************/
void help(char **argv)
{
    char *phelp  = usagetext;   // default help is just usage

    // print help based on how/which command was invoked
    if (!strcmp("edlist", argv[0]))
        phelp = helplist;
    else if (!strcmp("edset", argv[0]))
        phelp = helpset;
    else if (!strcmp("edget", argv[0]))
        phelp = helpget;
    else if (!strcmp("edcat", argv[0]))
        phelp = helpcat;

    printf("%s", phelp);

    return;
}


/**************************************************************
 * help text
 **************************************************************/
char helplist[] = "\n\
The edlist command prints a list of every plug-in in\n\
every slot in the system.  The output shows the plug-in\n\
slot number, the name of the plug-in, and a one line\n\
description of the plug-in.  Below the description of\n\
each plug-in is a list of its resources and which commands\n\
work with those resources.  Request more information about a\n\
particular plug-in by entering edlist followed by the name\n\
of the plug-in.  For example:\n\
  edlist hellodemo\n\
\n";

char helpget[] = "\n\
The edget command retrieves the value of a resource from a\n\
plug-in.  Required parameters include the slot number (or\n\
plug-in name), and the name of the resource.  The return\n\
value is an ASCII string terminated by a newline.  The format\n\
of the data in the string depends on the resource being queried.\n\
For example, the following commands both get the value of the\n\
buttons on the Baseboard\n\
    edget hellodemo period\n\
    edget 1 period\n\
\n";

char helpset[] = "\n\
The edset command writes a new value into a resource of a\n\
plug-in.  Required parameters include the slot number (or\n\
plug-in name), the name of the resource, and the new value\n\
of the resource.  The new value is an ASCII string, the format of\n\
which depends on the resource being changed.  Use edlist to get\n\
a list of the plug-in and resources available.  For example,\n\
the following commands both set the value of the eight LEDs on\n\
the Baseboard\n\
    edset hellodemo period 5\n\
\n";

char helpcat[] = "\n\
The edcat command connects to a stream of sensor or input device\n\
readings from a resource.  Required parameters include the slot\n\
number (or plug-in name) and the name of the resource.  Once a\n\
edcat command is issued the process remains running until explicitly\n\
killed.  Not all sensors or input resources can broadcast readings\n\
(look for edcat in the edlist output),  and the edcat output format\n\
is dependent on the type of resource.  Use 'edlist <pluginname>'\n\
to see the data format for the resource output for a given plug-in.\n\
The underlying mechanism for edcat is a TCP connection to the empty daemon.\n\
The most common use of edcat at the empty daemon protocol level is to\n\
combine it with select() to form an event driven system.  You may wish\n\
to use edcat with all of the inputs and sensors in your system.  The\n\
buttons on the Baseboard can be used with edcat.  The command is\n\
    edcat hellodemo message\n\
\n";

char usagetext[] = "\
Usage is command specific.  Empty daemon command syntaxes are as follows:\n\
  edset <slot#|plug-in_name> <resourcename> <value(s)>\n\
  edget <slot#|plug-in_name> <resourcename>\n\
  edcat <slot#|plug-in_name> <resourcename>\n\
  edlist [plug-in_name]\n\
\n\
 options:\n\
 -p,        Specify TCP port of empty daemon.\n\
 -a,        Specify TCP address of empty daemon.  Default is 127.0.0.1\n\
 -h,        Print full help for the given empty daemon command.\n\
";

