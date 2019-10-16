/*
 *  Name: dc2.c
 *
 *  Description: Driver for a dual DC motor controller
 *
 *  Resource Summary:
 *    pwm_frequency - PWM frequency in Hertz
 *    mode         - (B)rake, (C)oast, (F)orward, (R)everse
 *    power        - PWM pulse width as percentage
 *    watchdog     - msec between mode/power update to prevent autostop
 *
 *  Resources:
 *    The device interfaces to the dual H-bridge controller offer
 *  independent power, direction, brake or coast mode for each of
 *  the two motors.  The PWM frequency is common to both motors.
 *
 *  mode0, mode1
 *    The mode of operation for the motors.  Three modes of operation
 *  are available: forward, reverse, and brake.  Writing a single
 *  character 'f', 'r', or 'b' to this resource sets the mode.
 *
 *  power0, power1
 *    The PWM "ON" time as a percentage in the range 0 percent
 *  to 100 percent.   The power resolution is 10 bits for a PWM
 *  frequency of 20 KHz and 8 bits for a PWM frequency of 78 KHz.
 *
 *  pwm_frequency
 *    The frequency of the power PWM signal in Hertz.  The driver
 *  tries to assign a frequency as close as possible to the one
 *  specified.  For example, a requested frequency of 12510 Hertz
 *  might result in an actual PWM frequency of 12500 Hertz.  The
 *  PWM frequency is the same for both motors.
 *
 *  watchdog
 *     As a safety feature, the driver and FPGA hardware can turn both
 *  motors off if there is no watchdog update from the controlling
 *  software within the specified time.  The time is specified in
 *  milliseconds and has a maximum value of 1500 milliseconds.  The
 *  resolution of the watchdog timer is 100 milliseconds, and a value
 *  of zero turns off the watchdog timer.  The default value is zero.
 *
 */

/*
 *  Registers:
 *  The FPGA peripheral provides direction, PWM power control, and both brake
 *  and coast for two motors using four FPGA pins. The motors are numbered
 *  0 and 1 and the two motor lines are called A and B.  The lowest numbered
 *  pin on the connector is the A line for motor 0 and the second pin is the
 *  B line. Pins 3 and 4 are the A and B lines for motor number 1.  The modes
 *  of operation are as follows:
 *           MODE        B          A
 *      (0) Coast        low        low
 *      (1) Reverse      low        (PWM)
 *      (2) Forward      (PWM)      low
 *      (3) Brake        high       high        The power-on default
 *
 *  This PWM motor controller uses a 10-bit "period" counter that counts from
 *  up from 1 up to (period -1).  This (1...N-1) lets the PWM output have both
 *  0 and 100 percent PWM pulse widths.  The count rate is selected by the
 *  "clksel" field which is the high three bits of register 0.  The period is
 *  split between the low 2 bits of register 0 and all 8 bits of register 1.
 *
 *  The clock source is selected by the upper 3 bits of register 0:
 *      0:  Off
 *      1:  20 MHz
 *      2:  10 MHz
 *      3:  5 MHz
 *      4:  1 MHz
 *      5:  500 KHz
 *      6:  100 KHz
 *      7:  50 KHz
 *  Bit 2 and 3 of register 0 set the mode during the off part of the PWM
 *  cycle.  
 *
 *  Register 2/3 uses the high two bits for motor 0 mode and the low ten
 *  bits for the "on" count for the PWM output on 0.  Smaller values of on
 *  count turn the output on sooner and so cause the motor to turn faster.
 *
 *  The motor 1 output goes high at the start of the cycle.  The motor 1 
 *  turn off count is in the low ten bits of register 4.  The high two bits
 *  of register 4 is the motor 1 mode.  Full motor off is when the off count
 *  equals zero.  Full motor on is when the count equals the period.
 *
 *  A note of explanation: So motor 0 is on from the _end_ of the period to
 *  a time specified in registers 2/3, and motor 1 is on from the _start_ the
 *  period.  This seems counter intuitive but serves a purpose.  It serves to
 *  minimize the time that _both_ motors are on and this can reduce I2R losses
 *  in the cables and battery.  This is subtle but can slightly extend the
 *  battery charge.
 *
 *  Register 6 is the watchdog control register.  The idea of the watchdog is
 *  that if enabled (bit 7 == 1) the low four bits are decremented once every
 *  100 millisecond.  If the watchdog count reaches zero both PWM outputs are
 *  turned off.  A four bit counter gives a minimum update rate of about once
 *  every 1.5 seconds.  Just rewriting the same value into the power or mode
 *  registers is enough to reset the watchdog counter.
 */

/*
 * Copyright:   Copyright (C) 2014-2019 Demand Peripherals, Inc.
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
 *              Please contact Demand Peripherals if you wish to use this code
 *              in a non-GPLv2 compliant manner. 
 * 
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include "eedd.h"
#include "readme.h"


/**************************************************************
 *  - Limits and defines
 **************************************************************/
        // DC2 register definitions
#define DC2_REG_PWM             0 // encoded PWM for the motor controller
#define DC2_REG_MODEA           2 // encoded operating mode for motor A
#define DC2_REG_MODEB           4 // encoded operating mode for motor B
#define DC2_REG_WATCHDOG        6 // encoded watchdog control
        // resource names and numbers
#define FN_MODE0           "mode0"
#define FN_MODE1           "mode1"
#define FN_POWER0          "power0"
#define FN_POWER1          "power1"
#define FN_PWMFREQ         "pwm_frequency"
#define FN_WATCHDOG        "watchdog"
#define RSC_MODE0          0
#define RSC_MODE1          1
#define RSC_POWER0         2
#define RSC_POWER1         3
#define RSC_PWMFREQ        4
#define RSC_WATCHDOG       5
#define NUM_DC2_RSC        6
        // PWM register constants
#define CLKSEL_OFF              0 // clock off
#define CLKSEL_20MHZ            1 // 20MHz clksel code
#define CLKSEL_10MHZ            2 // 10MHz clksel code
#define CLKSEL_5MHZ             3 // 5MHz clksel code
#define CLKSEL_1MHZ             4 // 1MHz clksel code
#define CLKSEL_500KHZ           5 // 500KHz clksel code
#define CLKSEL_100KHZ           6 // 100KHz clksel code
#define CLKSEL_50KHZ            7 // 50KHz clksel code
        // Motor mode constants
#define MODE_COAST              0 // coast mode code
#define MODE_REVERSE            1 // reverse mode code
#define MODE_FORWARD            2 // forward mode code
#define MODE_BRAKE              3 // brake mode code (default)
        // Is PWM off coast, brake, or reverse ?
#define PWMOFF_COAST         0x00
#define PWMOFF_REVERS        0x04 // aka: locked-antiphase
#define PWMOFF_BRAKE         0x0c
#define PWMOFF_MODE  PWMOFF_BRAKE
        // misc constants
#define MAX_LINE_LEN          100 // max line length from the user
#define MAX_PERIOD           1023


/**************************************************************
 *  - Data structures
 **************************************************************/
    // Data kept for a single motor
typedef struct
{
    int      power;        // power in tenths of a percent (0 to 1000)
    int      mode;         // brake, coast, forward, or reverse
    struct DC2DEV *pDC2;   // The peripheral context for this motor
} MOTOR;

    // Context of this peripheral
typedef struct
{
    SLOT    *pslot;        // handle to peripheral's slot info
    void    *ptimer;       // timer to watch for dropped ACK packets
    int      pwmClkSel;    // Clock source: 20 MHz down to 50 KHz
    int      pwmPeriod;    // PWM period in units of clksel above. Range is 1 to 1022
    int      pwmFreq;      // PWM frequency (computed based on period and clksel)
    int      dog_time;     // Set to watchdog timeout time in tenths of a second
    MOTOR    ch0;          // context of motor 0
    MOTOR    ch1;          // context of motor 1
} DC2DEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void packet_hdlr(SLOT *, DP_PKT *, int);
static void userpwmfreq(int, int, char*, SLOT*, int, int*, char*);
static void userwatchdog(int, int, char*, SLOT*, int, int*, char*);
static void usermode(int, int, char*, SLOT*, int, int*, char*);
static void userpower(int, int, char*, SLOT*, int, int*, char*);
static void noAck(void *, DC2DEV *);
static void sendconfigtofpga(DC2DEV *, int *plen, char *buf);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    DC2DEV *pctx;      // our local device context
    int     i;         // generic loop counter

    // Allocate memory for this peripheral
    pctx = (DC2DEV *) malloc(sizeof(DC2DEV));
    if (pctx == (DC2DEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in dc2 initialization");
        return (-1);
    }

    // Init our DC2DEV structure
    pctx->pslot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    pslot->rsc[RSC_MODE0].name = FN_MODE0;
    pslot->rsc[RSC_MODE0].pgscb = usermode;
    pslot->rsc[RSC_MODE1].name = FN_MODE1;
    pslot->rsc[RSC_MODE1].pgscb = usermode;
    pslot->rsc[RSC_PWMFREQ].name = FN_PWMFREQ;
    pslot->rsc[RSC_PWMFREQ].pgscb = userpwmfreq;
    pslot->rsc[RSC_POWER0].name = FN_POWER0;
    pslot->rsc[RSC_POWER0].pgscb = userpower;
    pslot->rsc[RSC_POWER1].name = FN_POWER1;
    pslot->rsc[RSC_POWER1].pgscb = userpower;
    pslot->rsc[RSC_WATCHDOG].name = FN_WATCHDOG;
    pslot->rsc[RSC_WATCHDOG].pgscb = userwatchdog;
    // Common resource settings
    for (i = 0; i < MX_RSC; i++) {
        pslot->rsc[i].flags = IS_READABLE | IS_WRITABLE;
        pslot->rsc[i].bkey = 0;
        pslot->rsc[i].uilock = -1;
        pslot->rsc[i].slot = pslot;
    }
    pslot->name = "dc2";
    pslot->desc = "Dual DC motor controller";
    pslot->help = README;


    // init PWM frequency to 20 KHz, mode to break, and power to 0
    pctx->pwmClkSel = CLKSEL_20MHZ;
    pctx->pwmPeriod = 1000;
    pctx->pwmFreq = 20000;
    pctx->ch0.mode = MODE_BRAKE;
    pctx->ch0.power = 0;
    pctx->ch0.pDC2 = (struct DC2DEV *) pctx;
    pctx->ch1.mode = MODE_BRAKE;
    pctx->ch1.power = 0;
    pctx->ch1.pDC2 = (struct DC2DEV *) pctx;
    pctx->dog_time = 0;  // disable watchdog timer

    // Send the value, direction and interrupt setting to the card.
    // Ignore return value since there's no user connection and
    // system errors are sent to the logger.
    sendconfigtofpga(pctx, (int *) 0, (char *) 0);  // send modes, powers, dogtime

    return (0);
}

/**************************************************************
 * packet_hdlr():  - Handle incoming packets from the FPGA board
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    DC2DEV *pctx;      // our local info

    pctx = (DC2DEV *)(pslot->priv);  // Our "private" data is a DC2DEV

    // Clear the timer on write response packets
    if ((pkt->cmd & DP_CMD_OP_MASK) == DP_CMD_OP_WRITE) {
        if (pctx->ptimer) {
            del_timer(pctx->ptimer);  // Got the ACK
            pctx->ptimer = 0;
        }
        return;
    }

    // There are no other packets from the dc2 FPGA code so if we
    // get here there is a problem.  Log the error.
    edlog("invalid dc2 packet from board to host");

    return;
}


/**************************************************************
 * userpwmfreq():  - The user is reading or setting the PWM frequency.
 **************************************************************/
static void userpwmfreq(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    DC2DEV  *pctx;     // our local info
    int      ret;      // return count
    int      newfreq;  // new PWM frequency

    pctx = (DC2DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%d\n", pctx->pwmFreq);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        // read PWM frequency from user
        ret = sscanf(val, "%d", &newfreq);
        if (ret != 1) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // Determine the base freq and period with which to synthesize the new freq
        // We want as many bits as possible but the period counter must be less than
        // the MAX_PERIOD.  Start low and search high to get the most bits in period.
        if (newfreq == 0) {
            pctx->pwmClkSel = CLKSEL_OFF;
            pctx->pwmPeriod = 0;
            pctx->pwmFreq = 0;
        }
        else if (newfreq > (20000000 / MAX_PERIOD)) {
            pctx->pwmClkSel = CLKSEL_20MHZ;
            pctx->pwmPeriod = 20000000 / newfreq;
            pctx->pwmFreq = 20000000 / pctx->pwmPeriod;
        }
        else if (newfreq > (10000000 / MAX_PERIOD)) {
            pctx->pwmClkSel = CLKSEL_10MHZ;
            pctx->pwmPeriod = 10000000 / newfreq;
            pctx->pwmFreq = 10000000 / pctx->pwmPeriod;
        }
        else if (newfreq > (5000000 / MAX_PERIOD)) {
            pctx->pwmClkSel = CLKSEL_5MHZ;
            pctx->pwmPeriod = 5000000 / newfreq;
            pctx->pwmFreq = 5000000 / pctx->pwmPeriod;
        }
        else if (newfreq > (1000000 / MAX_PERIOD)) {
            pctx->pwmClkSel = CLKSEL_1MHZ;
            pctx->pwmPeriod = 1000000 / newfreq;
            pctx->pwmFreq = 1000000 / pctx->pwmPeriod;
        }
        else if (newfreq > (500000 / MAX_PERIOD)) {
            pctx->pwmClkSel = CLKSEL_500KHZ;
            pctx->pwmPeriod = 500000 / newfreq;
            pctx->pwmFreq = 500000 / pctx->pwmPeriod;
        }
        else if (newfreq > (100000 / MAX_PERIOD)) {
            pctx->pwmClkSel = CLKSEL_100KHZ;
            pctx->pwmPeriod = 100000 / newfreq;
            pctx->pwmFreq = 100000 / pctx->pwmPeriod;
        }
        else {
            pctx->pwmClkSel = CLKSEL_50KHZ;
            pctx->pwmPeriod = 50000 / newfreq;
            pctx->pwmFreq = 50000 / pctx->pwmPeriod;
        }
    }

    // Send out new motor config
    sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr

    return;
}


/**************************************************************
 * userwatchdog():  - The user is reading or setting the watchdog
 * timeout.
 **************************************************************/
static void userwatchdog(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    DC2DEV  *pctx;     // our local info
    int      ret;      // return count
    int      newdog;   // new value to assign the watchdog timer

    pctx = (DC2DEV *) pslot->priv;

    if (cmd == EDGET) {
        ret = snprintf(buf, *plen, "%d\n", (100 * pctx->dog_time));
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        ret = sscanf(val, "%d", &newdog);
        if ((ret != 1) || (newdog < 0) || (newdog > 1500) ||
            (newdog % 100 != 0)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }
        pctx->dog_time = (unsigned short) newdog / 100;
        sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr
    }

    return;
}


/**************************************************************
 * usermode():  - The user is reading or setting a motor's mode
 **************************************************************/
static void usermode(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    DC2DEV  *pctx;     // our local info
    int      ret;      // return count
    MOTOR   *pMot;     // motor to view/change

    pctx = (DC2DEV *) pslot->priv;
    pMot = (rscid == RSC_MODE0) ? &(pctx->ch0) : &(pctx->ch1);

    if (cmd == EDGET) {
        // write mode character to the user
        if (pMot->mode == MODE_FORWARD) {
            strcpy(buf, "f\n");
            *plen = 2;
        }
        else if (pMot->mode == MODE_REVERSE) {
            strcpy(buf, "r\n");
            *plen = 2;
        }
        else if (pMot->mode == MODE_BRAKE) {
            strcpy(buf, "b\n");
            *plen = 2;
        }
        else if (pMot->mode == MODE_COAST) {
            strcpy(buf, "c\n");
            *plen = 2;
        }
        else {
            ret = snprintf(buf, *plen, "Unknown motor mode\n");
            *plen = ret;  // (errors are handled in calling routine)
            return;
        }
        return;
    }
    else if (cmd == EDSET) {
        switch (tolower(val[0])) {
            case 'f' :
                pMot->mode = MODE_FORWARD;
                break;
            case 'r' :
                pMot->mode = MODE_REVERSE;
                break;
            case 'b' :
                pMot->mode = MODE_BRAKE;
                break;
            case 'c' :
                pMot->mode = MODE_COAST;
                break;
            default :
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                break;
        }
    }

    // Send out new motor config
    sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr

    return;
}


/**************************************************************
 * userpower():  - The user is reading or setting a motor's power
 **************************************************************/
static void userpower(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    DC2DEV *pctx;      // our local info
    int      ret;      // return count
    int      power;
    int      units, tenths;   // New target power
    MOTOR   *pMot;     // motor to view/change

    pctx = (DC2DEV *) pslot->priv;
    pMot = (rscid == RSC_POWER0) ? &(pctx->ch0) : &(pctx->ch1);

    if (cmd == EDGET) {
        ret =  snprintf(buf, MAX_LINE_LEN, "%d.%d\n",
               pMot->power/10, pMot->power % 10);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }
    else if (cmd == EDSET) {
        // read an unsigned decimal value from the user
        if (sscanf(val, "%u.%u", &units, &tenths) != 2) {
            if (sscanf(val, "%u", &units) != 1) {
                ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                *plen = ret;
                return;
            }
            tenths = 0;   // for single number entries like "50"
        }

        // qualify the value, 0 to 1000 of tenths of a percent
        power = (10 * units) + tenths;
        if ((units > 100) || (tenths > 9) || (power > 1000)) {
            ret = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            *plen = ret;
            return;
        }

        // do the power change (or update)
        pMot->power = power;
    }

    // Send out new motor config
    sendconfigtofpga(pctx, plen, buf);  // send pins, dir, intr

    return;
}



/**************************************************************
 * sendconfigtofpga():  - Send motor configuration to the FPGA card.
 * Put error messages into buf and update plen.
 **************************************************************/
static void sendconfigtofpga(
    DC2DEV *pctx,    // This peripheral's context
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)      // where to store user visible error messages
{
    DP_PKT   pkt;      // send write and read cmds to the dc2
    SLOT    *pslot;    // our slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      ret;      // generic return value
    int      pwm0;     // Convert the currentSpeed to a PWM width for motor 0
    int      pwm1;     // Convert the currentSpeed to a PWM width for motor 1


    pslot = pctx->pslot;
    pmycore = pslot->pcore;

    // create a write packet to set the value of the register
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;
    pkt.reg = DC2_REG_PWM;
    pkt.count = 8;
    pkt.data[0] = (pctx->pwmClkSel << 5) | PWMOFF_MODE | (pctx->pwmPeriod >> 8);
    pkt.data[1] = pctx->pwmPeriod & (0x00FF);

    /* Convert current power for motor #0 to a PWM pulse width */
    pwm0 = ((1000 - pctx->ch0.power) * pctx->pwmPeriod) / 1000;
    pkt.data[2] = (pctx->ch0.mode << 6) | (pwm0 >> 8);
    pkt.data[3] = pwm0 & (0x00FF);

    /* Convert current power for motor #1 to a PWM pulse width */
    pwm1 = (pctx->ch1.power * pctx->pwmPeriod) / 1000;
    pkt.data[4] = (pctx->ch1.mode << 6) | (pwm1 >> 8);
    pkt.data[5] = pwm1 & (0x00FF);

    /* Set the watchdog on/off and time */
    pkt.data[6] = (pctx->dog_time != 0) ? 0x80 : 0x00;
    pkt.data[7] = pctx->dog_time & 0x000F;

    // try to send the packet.  Apply or release flow control.
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    if (txret != 0) {
        // the send of the new pin values did not succeed.  This
        // probably means the input buffer to the USB port is full.
        // Tell the user of the problem.
        ret = snprintf(buf, *plen, E_WRFPGA);
        *plen = ret;  // (errors are handled in calling routine)
        return;
    }

    // Start timer to look for a write response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, noAck, (void *) pctx);

    return;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void noAck(
    void      *timer,   // handle of the timer that expired
    DC2DEV    *pctx)    // Send pin values of this dc2 to the FPGA
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}

// end of dc2.c
