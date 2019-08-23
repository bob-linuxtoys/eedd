/*
 *  Name: vl53.c
 *
 *  Description: Interface to VL53L0X time of flight range sensor
 *
 *  Resources:
 *    device -      full path to the I2C device for the VL53L0X (/dev/i2c-1)
 *    hw_rev -      model and revision of the range sensor.
 *    longrange -   enable long-range measurements
 *    period -      update interval in milliseconds
 *    distance -    broadcast for range measurements as they arrive
 */

/*
 * Copyright:   Copyright (C) 2019 by Demand Peripherals, Inc.
 *              All rights reserved.
 *
 *              Copyright (c) 2017 Larry Bank
 *              email: bitbank@pobox.com
 *              Project started 7/29/2017
 *
 * License:     This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License as 
 *              published by the Free Software Foundation, either version 3 of 
 *              the License, or (at your option) any later version.
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *              GNU General Public License for more details. 
 *
 *              You should have received a copy of the GNU General Public License
 *              along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <limits.h>             // for PATH_MAX
#include <linux/joystick.h>
#include "../include/eedd.h"
#include "readme.h"
#include "tof.h"                // time of flight sensor library


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // resource names and numbers
#define FN_DEVICE       "device"
#define FN_HWREV        "hwrev"
#define FN_LONGRANGE    "longrange"
#define FN_PERIOD       "period"
#define FN_RANGE        "range"
#define RSC_DEVICE      0
#define RSC_HWREV       1
#define RSC_LONGRANGE   2
#define RSC_PERIOD      3
#define RSC_RANGE       4
        // What we are is a ...
#define PLUGIN_NAME        "vl53"
        // device
#define DEFDEV             "/dev/i2c-1"
        // I2C device ID
#define I2C_DEV_ID         0x29
        // Maximum size of output string
#define MX_MSGLEN          120


/**************************************************************
 *  - Data structures
 **************************************************************/
    // All state info for an instance of a vl53
typedef struct
{
    void    *pslot;             // handle to plug-in's's slot info
    void    *ptimer;            // timer with callback to bcast distance
    int      i2c_channel;       // I2C channel (for Pi default is 1)
    char     device[PATH_MAX];  // full path to device node
    int      model;             // model of the HW
    int      revision;          // revision of the HW
    int      longrange;         // long range measurement enable flag, 0 or 1
    int      period;            // update period for sending distance measurement
    int      vl53fd;            // File Descriptor (=-1 if closed)
} VL53;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void usercmd(int, int, char*, SLOT*, int, int*, char*);
static void rangecb(int, void*);
void do_range(VL53*);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this plug-in
{
    VL53 *pctx;        // our local device context

    // Allocate memory for this plug-in
    pctx = (VL53 *) malloc(sizeof(VL53));
    if (pctx == (VL53 *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in vl53 initialization");
        return (-1);
    }

    // Init our VL53 structure
    pctx->pslot = pslot;        // this instance of the hello demo
    pctx->period = 100;         // default period of measurements
    (void) strncpy(pctx->device, DEFDEV, PATH_MAX);
    pctx->longrange = 1;        // set long range mode (up to 2m)

    // TODO: currently only a single instance of the TOF sensor can be used
    // now open and register the vl53 I2C device
    sscanf(pctx->device, "/dev/i2c-%d", &pctx->i2c_channel);
    pctx->vl53fd = tofInit(pctx->i2c_channel, I2C_DEV_ID, pctx->longrange);
    if (pctx->vl53fd != -1) {
	    tofGetModel(&pctx->model, &pctx->revision);
        add_fd(pctx->vl53fd, ED_READ, rangecb, (void *) pctx);
    }
    else
    {
        // TODO: what is the correct way to handle a bad open???
        edlog("device could not be opened");
        return (-1);
    }
    
    // Register name and private data
    pslot->name = PLUGIN_NAME;
    pslot->priv = pctx;
    pslot->desc = "VL53L0X Time of Flight Range Sensor";
    pslot->help = README;

    // Add handlers for the user visible resources
    pslot->rsc[RSC_DEVICE].name = FN_DEVICE;
    pslot->rsc[RSC_DEVICE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_DEVICE].bkey = 0;
    pslot->rsc[RSC_DEVICE].pgscb = usercmd;
    pslot->rsc[RSC_DEVICE].uilock = -1;
    pslot->rsc[RSC_DEVICE].slot = pslot;
    pslot->rsc[RSC_HWREV].name = FN_HWREV;
    pslot->rsc[RSC_HWREV].flags = IS_READABLE;
    pslot->rsc[RSC_HWREV].bkey = 0;
    pslot->rsc[RSC_HWREV].pgscb = usercmd;
    pslot->rsc[RSC_HWREV].uilock = -1;
    pslot->rsc[RSC_HWREV].slot = pslot;
    pslot->rsc[RSC_LONGRANGE].name = FN_LONGRANGE;
    pslot->rsc[RSC_LONGRANGE].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_LONGRANGE].bkey = 0;
    pslot->rsc[RSC_LONGRANGE].pgscb = usercmd;
    pslot->rsc[RSC_LONGRANGE].uilock = -1;
    pslot->rsc[RSC_LONGRANGE].slot = pslot;
    pslot->rsc[RSC_PERIOD].name = FN_PERIOD;
    pslot->rsc[RSC_PERIOD].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_PERIOD].bkey = 0;
    pslot->rsc[RSC_PERIOD].pgscb = usercmd;
    pslot->rsc[RSC_PERIOD].uilock = -1;
    pslot->rsc[RSC_PERIOD].slot = pslot;
    pslot->rsc[RSC_RANGE].name = FN_RANGE;
    pslot->rsc[RSC_RANGE].flags = CAN_BROADCAST;
    pslot->rsc[RSC_RANGE].bkey = 0;
    pslot->rsc[RSC_RANGE].pgscb = 0;
    pslot->rsc[RSC_RANGE].uilock = -1;
    pslot->rsc[RSC_RANGE].slot = pslot;

    // Start the timer to broadcast state info
    if (pctx->period != 0)
        pctx->ptimer = add_timer(ED_PERIODIC, pctx->period, rangecb, (void *) pctx);
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
    VL53    *pctx;     // our local info
    int      ret;      // return count
    int      nlongrange;  // new value to assign to the filter
    int      nperiod;  // new value to assign to the period

    // point to the current context
    pctx = (VL53 *) pslot->priv;

    // get resource values
    if (cmd == EDGET)
    {
        switch (rscid)
        {
            case RSC_DEVICE:
                ret = snprintf(buf, *plen, "%s\n", pctx->device);
                *plen = ret;  // (errors are handled in calling routine)
                break;
                
            case RSC_HWREV:
                ret = snprintf(buf, *plen, "%d %d\n", pctx->model, pctx->revision);
                *plen = ret;  // (errors are handled in calling routine)
                break;
                
            case RSC_LONGRANGE:
                ret = snprintf(buf, *plen, "%d\n", pctx->longrange);
                *plen = ret;  // (errors are handled in calling routine)
                break;
            
            case RSC_PERIOD:
                ret = snprintf(buf, *plen, "%d\n", pctx->period);
                *plen = ret;  // (errors are handled in calling routine)
                break;
        }
    }
    
    // set resource values
    else
    {
        switch (rscid)
        {
            case RSC_DEVICE:

                // Val has the new device path.  Just copy it.
                (void) strncpy(pctx->device, val, PATH_MAX);
                
                // strncpy() does not force a null.  We add one now as a precaution
                pctx->device[PATH_MAX -1] = (char) 0;
      
                // verify device name
                if (sscanf(pctx->device, "/dev/i2c-%d", &pctx->i2c_channel) < 1) {
                    ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                    *plen = ret;  // (errors are handled in calling routine)
                    return;
                }
                
                // close and unregister the old device
                if (pctx->vl53fd >= 0) {
                    del_fd(pctx->vl53fd);
                    close(pctx->vl53fd);
                    pctx->vl53fd = -1;
                }
                
                // now open and register the new device
                pctx->vl53fd = tofInit(pctx->i2c_channel, I2C_DEV_ID, pctx->longrange);
                if (pctx->vl53fd != -1) {
	                tofGetModel(&pctx->model, &pctx->revision);
                    add_fd(pctx->vl53fd, ED_READ, rangecb, (void *) pctx);
                }
                else
                {
                    // TODO: what to do on bad open???
                }
                
                break;
                
            case RSC_LONGRANGE:
            
                // parse and verify value
                ret = sscanf(val, "%d", &nlongrange);
                if ((ret != 1) || (nlongrange < 0) || (nlongrange > 1)) {
                    ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                    *plen = ret;  // (errors are handled in calling routine)
                    return;
                }
                
                // record the new value
                pctx->longrange = nlongrange;

                // close and unregister the old device
                if (pctx->vl53fd >= 0) {
                    del_fd(pctx->vl53fd);
                    close(pctx->vl53fd);
                    pctx->vl53fd = -1;
                }
                
                // now open and register the new device
                pctx->vl53fd = tofInit(pctx->i2c_channel, I2C_DEV_ID, pctx->longrange);
                if (pctx->vl53fd != -1) {
	                tofGetModel(&pctx->model, &pctx->revision);
                    add_fd(pctx->vl53fd, ED_READ, rangecb, (void *) pctx);
                }
                else
                {
                    // TODO: what to do on bad open???
                }
                
                break;
                
            case RSC_PERIOD:
            
                // FIXME: a period of 50 gives the following error:
                //        "eddaemon: Missed TO on 0.  Rescheduling"
                // parse and verify value
                ret = sscanf(val, "%d", &nperiod);
                if ((ret != 1) || (nperiod < 0) || (nperiod > 5000)) {
                    ret = snprintf(buf, *plen, E_BDVAL, pslot->rsc[rscid].name);
                    *plen = ret;  // (errors are handled in calling routine)
                    return;
                }
                
                // record the new value
                pctx->period = nperiod;

                // delete old timer and create a new one with the new period
                if (pctx->ptimer) {
                    del_timer(pctx->ptimer);
                }
                if (pctx->period != 0) {
                    pctx->ptimer = add_timer(ED_PERIODIC, pctx->period, rangecb, (void *) pctx);
                }
                
                break;
        }
    }
    
    return;
}


/***************************************************************************
 *  rangecb()  - Handle range data from the vl53
 *
 ***************************************************************************/
void rangecb(
    int      fd,       // FD of device with data ready to read
    void    *priv)     // callback data
{
    VL53 *pctx = (VL53*)priv;

    // read a range value and broadcast it
	do_range(pctx);
    
    return;
}


/***************************************************************************
 *  do_range()  - process a value from the vl53
 *
 ***************************************************************************/
void do_range(VL53 *pctx)
{
    SLOT     *pslot;
    RSC      *prsc;    // pointer to this slot's counts resource
    int       range;   // the current range value
    char      lineout[MX_MSGLEN];  // output to send to users
    int       nout;    // length of output line

    // Get slot and pointer to range resource structure
    pslot = pctx->pslot;
    prsc = &(pslot->rsc[RSC_RANGE]);

    // read and broadcast the range value if anyone is listening
    if (prsc->bkey)
    {
        range = tofReadDistance();
        if (range < 4096)
        {
            // format the range value
	        snprintf(lineout, MX_MSGLEN, "%d\n", range);
            nout = strnlen(lineout, MX_MSGLEN-1);
            
            // bkey will return cleared if UIs are no longer monitoring us
            bcst_ui(lineout, nout, &(prsc->bkey));
        }
    }        

    return;
}


