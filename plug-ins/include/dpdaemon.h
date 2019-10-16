/*
 * Name: dpdaemon.h
 *
 * Description: This file contains the define's and data structures for use
 *              in the DP Daemon.
 *
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

#ifndef DPDAEMON_H_
#define DPDAEMON_H_

#include <stdint.h>

/***************************************************************************
 *  - Defines
 ***************************************************************************/
#define NUM_CORE        11      /* # peripherals per FPGA */
#define DP_PKTLEN       514     /* DP protocol packet size */
#define RXBUF_SZ       4000     /* Buffer size for USB packet reads */

#define DP_PKTLEN       514     // DP protocol packet size
#define PKT_DATA_SZ     510     // Max # bytes in packet payload

// DP protocol command bits
        // BIT 7: Rsp pkt type -- 1 = automatic data, 0 = read response
#define DP_CMD_AUTO_DATA    0x00
#define DP_CMD_AUTO_MASK    0x80
#define RESERVED_00         0x20  // Bit 5:   Reserved for future use
#define RESERVED_01         0x10  // Bit 4:   Reserved for future use
#define DP_CMD_OP_NOP       0x00  // Bit 3-2: 00 = No operation
#define DP_CMD_OP_READ      0x04  // Bit 3-2: 01 = Read data from the peripheral
#define DP_CMD_OP_WRITE     0x08  // Bit 3-2: 10 = Write data to the peripheral
        // Bit 3-2: 11 = Write-Read data to/from the peripheral. Used for SPI
#define DP_CMD_OP_WRRD      0x0C
#define DP_CMD_OP_MASK      0x0C
        // Bit 1: Increment register -- 1 = autoinc, 0 = do not alter destination
#define DP_CMD_AUTOINC      0x02
#define DP_CMD_NOAUTOINC    0x00
#define DP_CMD_INCMASK      0x02
        // Bit 0:   Word size -- 1 = 16 bits, 0 = 8 bits
        // DEPRECATED!  All registers are now 8 bits
//#define DP_CMD_WORD_SIZE_16 0x01
//#define DP_CMD_WORD_SIZE_8  0x00
//#define DP_CMD_SIZE_MASK    0X01

// SLIP Protocol characters
#define SLIP_END      ((unsigned char) 192)
#define SLIP_ESC      ((unsigned char) 219)
#define INPKT_END     ((unsigned char) 220)
#define INPKT_ESC     ((unsigned char) 221)



/***************************************************************************
 *  - Data structures
 ***************************************************************************/
    // A DP protocol packet without SLIP encoding
typedef struct {
    uint8_t cmd;      // read, write, autoinc, 8/16
    uint8_t core;     // index of peripheral in FPGA image
    uint8_t reg;      // Peripheral config/status register #
    uint8_t count;    // How many words to transfer
    uint8_t data[PKT_DATA_SZ];
} DP_PKT;

    // Per FPGA peripheral (core) information kept by each plug-in
typedef struct {
    int       slot_id;         // which eedd slot we're in
    int       core_id;         // which FPGA peripheral we are
    char     *soname;          // shared object file name for peripheral
    void     *penum;           // so core plug-ins can get to usbFd
    void    (*pcb) ();         // Callback for packet arrival
} CORE;


/***************************************************************************
 *  dpi_tx_pkt():  Send a packet to the board
 *  Return 0 on success or a negative error code
 *  Error Codes: -1, write would block, retry tx later
 *               -2, invalid input values
 ***************************************************************************/
int dpi_tx_pkt(
    CORE   *pcore,        // The core sending the packet
    DP_PKT *inpkt,        // The packet to send
    int     len);         // Number of bytes in the packet


/***************************************************************************
 *  - User visible error messages
 ***************************************************************************/
#define E_WRFPGA  "ERROR 100 : Error writing to the FPGA card. Is link overloaded?\n"
#define E_NOACK   "ERROR 101 : Missing ACK from the FPGA card. Is link overloaded?\n"


/***************************************************************************
 *  - Log messages
 ***************************************************************************/
#define M_NOREDIR     "cannot redirect %s to /dev/null"
#define M_NOCD        "chdir to / failed with error: %s"
#define M_NONULL      "/dev/null open failed with error: %s"
#define M_NOSEND      "Board %d: send returns %d when sending %d bytes, errno = %d"
#define M_NOREAD      "read error on: %s"
#define M_NOFORK      "fork failed: %s"
#define M_BADPKT      "invalid packet from board at %s"
#define M_BADSO       "invalid shared object name: %s"
#define M_BADPORT     "configure of %s failed with: %s"
#define M_NOBRD       "sending packet to non-existent board at %s"
#define M_NOSID       "setsid failed with error: %s"
#define M_BADSLIP     "SLIP protocol error on board at %s"
#define M_NOPORT      "open failed on port %s"
#define M_NOOPEN      "open of %s failed with error: %s"
#define M_BADSYMB     "unable to load symbol %s in %s"
#define M_NOMOREFD    "too many open file descriptors"
#define M_NOMEM       "unable to allocate memory in %s"
#define M_PKTDMP      "Packet Dump: %s"
#define M_BADSCHED    "Scheduler changes failed with error: %s"
#define M_BADMLOCK    "Memory page locking failed with error: %s"
#define M_NOUI        "No free UI sessions"
#define M_BADCONN     "Error accepting UI connection. errno=%d"


#endif /* DPDAEMON_H_ */

