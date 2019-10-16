/*
 *  Name: avr.c
 *
 *  Description: Driver for the SPI peripheral
 *
 *  Hardware Registers:
 *    Addr=0    Clock select, chip select control, interrupt control and
 *              SPI mode register
 *    Addr=1    Max addr of packet data (== SPI pkt sz + 1)
 *    Addr=2    Data byte #1 in/out
 *    Addr=3    Data byte #2 in/out
 *    Addr=4    Data byte #3 in/out
 *        ::              ::
 *    Addr=14   Data byte #13 in/out
 *    Addr=15   Data byte #14 in/out
 *
 *  NOTES:
 *   - The RAM addresses are numbered from zero and the first two locations
 *     are mirrors of the two config registers.  Thus the actual SPI packet
 *     data starts at addr=2 and goes up to (SPI_pkt_sz + 1).  This means
 *     that at most 14 bytes can be sent at one time.
 *   - Extend the number of bytes in a packet by forcing CS low and sending
 *     several packets.  The electronics will see just one packet.
 *
 *  Resources:
 *    data      - read/write resource to send/receive SPI data
 *    config    - SPI port configuration including clock speed, and CS config
 *
 *
 * Copyright:   Copyright (C) 2015-2019 Demand Peripherals, Inc.
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
// register definitions
#define QCSPI_REG_MODE     0x00
#define QCSPI_REG_COUNT    0x01
#define QCSPI_REG_SPI      0x02
#define QCSPI_NDATA_BYTE   16   // num data registers from QCSPI_REG_SPI

// ESPI definitions
#define CS_MODE_AL          0   // Active low chip select
#define CS_MODE_AH          1   // Active high chip select
#define CS_MODE_FL          2   // Forced low chip select
#define CS_MODE_FH          3   // Forced high chip select
#define CLK_2M              0   // 2 MHz
#define CLK_1M              1   // 1 MHz
#define CLK_500K            2   // 500 KHz
#define CLK_100K            3   // 100 KHz

// misc constants
#define MAX_LINE_LEN        100
#define SIGNATURE_LEN       3

// resource names
#define FN_CFG              "config"
#define FN_DATA             "data"
#define FN_SIGNATURE        "signature"
#define FN_PROGRAM          "program"
#define FN_EEPROM           "eeprom"
#define FN_RAM              "vram"
#define FN_REG              "reg"
#define FN_FIFO             "fifo"

// Resource IDs
enum RscIds
{
    RSC_CFG = 0,
    RSC_DATA,
    RSC_SIGNATURE,
    RSC_PROGRAM,
    RSC_EEPROM,
    RSC_RAM,
    RSC_REG,
    RSC_FIFO
};

// resource handler IDs
enum TaskIds
{
    TASK_DEFAULT = 0,
    TASK_SIGNATURE,
    TASK_PROGRAM_GET,
    TASK_PROGRAM_SET,
    TASK_EEPROM_GET,
    TASK_EEPROM_SET,
    TASK_DATA_GET
};

// returned packet reply data offsets
#define REPLY_DATA_BYTE0    2
#define REPLY_DATA_BYTE1    3
#define REPLY_DATA_BYTE2    4
#define REPLY_DATA_BYTE3    5
#define REPLY_DATA_BYTE4    6

// UI aliases
#define REG_INDEX cmdLineArgv[0]
#define DATA_VAL(di) (cmdLineArgv[di+1])

// programming constants
#define SZ_32K 32768
#define MXLN   100

#define ATMEGA88
//#define ATMEGA328

#ifdef ATMEGA88
#define MXPG        128         // maximum number of program memory pages
#define PGSZ        64          // program memory page size in bytes -- 2 bytes/word (32 words)
#endif

#ifdef ATMEGA328
#define MXPG        256         // maximum number of program memory pages
#define PGSZ        128         // program memory page size in bytes -- 2 bytes/word (64 words)
#endif

#define PMEMSZ      MXPG*PGSZ   // program memory size in bytes
#define EEPROMSZ    0x200       // EEPROM size in bytes


/*
 * AVR Serial Programming Instruction Set

 * Instruction/Operation
 * Programming Enable                      $AC $53 $00 $00
 * Chip Erase (Program Memory/EEPROM)      $AC $80 $00 $00
 * Poll RDY/BSY                            $F0 $00 $00 data byte out

 * Load Instructions
 * Load Extended Address byte (1)          $4D $00 <Extended adr> $00
 * Load Program Memory Page, High byte     $48 $00 <adr LSB> <high data byte in>
 * Load Program Memory Page, Low byte      $40 $00 <adr LSB> <low data byte in>
 * Load EEPROM Memory Page (page access)   $C1 $00 000000aa <data byte in>

 * Read Instructions
 * Read Program Memory, High byte          $28 <adr MSB> <adr LSB> <high data byte out>
 * Read Program Memory, Low byte           $20 <adr MSB> <adr LSB> <low data byte out>
 * Read EEPROM Memory                      $A0 000000aa aaaaaaaa <data byte out>
 * Read Signature Byte                     $30 $00 0000 000aa <data byte out>

 * Write Instructions
 * Write Program Memory Page               $4C <adr MSB> <adr LSB> $00
 * Write EEPROM Memory                     $C0 000000aa aaaaaaaa <data byte in>
 * Write EEPROM Memory Page (page access)  $C2 000000aa aaaaaa00 $00
 */

typedef struct
{
    unsigned char opcode;
    unsigned char opnd1;
    unsigned char opnd2;
    unsigned char opnd3;
} INSTR;

#define OP_PROGRAM_ENABLE        0
#define OP_ERASE                 1
#define OP_LOAD_PMEM_PG_LO_BYTE  2
#define OP_LOAD_PMEM_PG_HI_BYTE  3
#define OP_LOAD_PROM_PG          4
#define OP_READ_PMEM_PG_LO_BYTE  5
#define OP_READ_PMEM_PG_HI_BYTE  6
#define OP_READ_EEPROM           7
#define OP_READ_SIG_BYTE         8
#define OP_WRITE_PMEM_PG         9
#define OP_WRITE_EEPROM         10
#define OP_WRITE_EEPROM_PG      11
INSTR InstructionSet[] =
{
    {0xAC, 0x53, 0x00, 0x00},   // program enable
    {0xAC, 0x80, 0x00, 0x00},   // erase
    {0x40, 0x00, 0x00, 0x00},   // Load Program Memory Page, Low byte
    {0x48, 0x00, 0x00, 0x00},   // Load Program Memory Page, High byte
    {0xC1, 0x00, 0x00, 0x00},   // Load EEPROM Memory Page (page access)
    {0x20, 0x00, 0x00, 0x00},   // read Program Memory Page, Low byte
    {0x28, 0x00, 0x00, 0x00},   // read Program Memory Page, High byte
    {0xA0, 0x00, 0x00, 0x00},   // Read EEPROM Memory
    {0x30, 0x00, 0x00, 0x00},   // read signature byte
    {0x4C, 0x00, 0x00, 0x00},   // write program memory page
    {0xC0, 0x00, 0x00, 0x00},   // Write EEPROM Memory
    {0xC2, 0x00, 0x00, 0x00}    // Write EEPROM Memory Page
};

// AVR data memory operations
#define OP_RD       0x00
#define OP_WR       0x01
#define OP_MEM      0x00
#define OP_REG      0x02
#define OP_MEM_RD   OP_MEM | OP_RD
#define OP_MEM_WR   OP_MEM | OP_WR
#define OP_REG_RD   OP_REG | OP_RD
#define OP_REG_WR   OP_REG | OP_WR
#define OP_AUTOINC  0x04

// hex file record constants
#define RECORD_DATA_SIZE 0x10
#define RECORD_TYPE_DATA 00

// per-quadrature clocked SPI port info
typedef struct
{
    int      csmode;        // active high/low or forced high/low
    int      clksrc;        // The SCK frequency
    struct AVRDEV *pAvr;    // peripheral for this spiport instance
} SPIPORT;

// espi local context
typedef struct
{
    SLOT    *pSlot;         // handle to peripheral's slot info
    int      flowCtrl;      // ==1 if we are applying flow control
    int      xferpending;   // ==1 if we are waiting for a reply
    void    *ptimer;        // Watchdog timer to abort a failed transfer
    int      nbxfer;        // Number of bytes in most recent SPI packet sent
    unsigned char bxfer[QCSPI_NDATA_BYTE]; // the bytes to send
    SPIPORT  spiport;       // spi port dev info
    SPIPORT  origSpiport;   // spi port dev info
    unsigned taskId;        // a task is associated with a sequence of instructions
    unsigned taskState;     // the current state of the task being performed
    char     filename[MAX_LINE_LEN];    // name of the file to program
    FILE    *pFile;         // input file pointer
    unsigned char *pbuf;    // memory to hold the program image
    int      imsz;          // size of image read from hex file
    unsigned char signature[3]; // signature buffer
    int      page;          // memory page counter
    int      pageAddr;      // byte address of current page
    INSTR    instruction;   // the current instruction being executed
    int      count;         // general purpose counter used to repeat instruction exec
    int      eepromAddr;    // beginning address for EEPROM access
} AVRDEV;


/**************************************************************
 *  - Function prototypes
 **************************************************************/
static void  packet_hdlr(SLOT *, DP_PKT *, int);
static void  Restore(RSC*, AVRDEV*);
static void  cb_data(int, int, char*, SLOT*, int, int*, char*);
static void  cb_config(int, int, char*, SLOT*, int, int*, char*);
static void  cb_program_mode(int, int, char*, SLOT*, int, int*, char*);
static void  cb_data_mode(int, int, char*, SLOT*, int, int*, char*);
static int   send_instruction(AVRDEV*, INSTR);
static int   send_spi(AVRDEV*);
static void  no_ack(void *, AVRDEV*);
static int   get_pgm_image(unsigned char*, int, char*);
static int   a2h(char);
static int   put_pgm_image(unsigned char*, int, char*);
static int   parse_ui(char*, unsigned char[], int*);
static void  return_ui(unsigned char[], int, RSC*);
extern int  dpi_tx_pkt(CORE *pcore, DP_PKT *inpkt, int len);


/**************************************************************
 * Initialize():  - Allocate our permanent storage and set up
 * the read/write callbacks.
 **************************************************************/
int Initialize(
    SLOT *pslot)       // points to the SLOT for this peripheral
{
    AVRDEV *pctx;    // our local device context

    // Allocate memory for this peripheral
    pctx = (AVRDEV *) malloc(sizeof(AVRDEV));
    if (pctx == (AVRDEV *) 0) {
        // Malloc failure this early?
        edlog("memory allocation failure in espi initialization");
        return (-1);
    }

    pctx->pSlot = pslot;       // our instance of a peripheral
    pctx->ptimer = 0;          // set while waiting for a response


    // Register this slot's packet handler and private data
    (pslot->pcore)->pcb  = packet_hdlr;
    pslot->priv = pctx;

    // Add the handlers for the user visible resources
    // base SPI peripheral resources
    pslot->rsc[RSC_DATA].name = FN_DATA;
    pslot->rsc[RSC_DATA].flags = IS_READABLE;
    pslot->rsc[RSC_DATA].bkey = 0;
    pslot->rsc[RSC_DATA].pgscb = cb_data;
    pslot->rsc[RSC_DATA].uilock = -1;
    pslot->rsc[RSC_DATA].slot = pslot;
    pslot->rsc[RSC_CFG].name = FN_CFG;
    pslot->rsc[RSC_CFG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_CFG].bkey = 0;
    pslot->rsc[RSC_CFG].pgscb = cb_config;
    pslot->rsc[RSC_CFG].uilock = -1;
    pslot->rsc[RSC_CFG].slot = pslot;

    // programming resources
    pslot->rsc[RSC_SIGNATURE].name = FN_SIGNATURE;
    pslot->rsc[RSC_SIGNATURE].flags = IS_READABLE;
    pslot->rsc[RSC_SIGNATURE].bkey = 0;
    pslot->rsc[RSC_SIGNATURE].pgscb = cb_program_mode;
    pslot->rsc[RSC_SIGNATURE].uilock = -1;
    pslot->rsc[RSC_SIGNATURE].slot = pslot;
    pslot->rsc[RSC_PROGRAM].name = FN_PROGRAM;
    pslot->rsc[RSC_PROGRAM].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_PROGRAM].bkey = 0;
    pslot->rsc[RSC_PROGRAM].pgscb = cb_program_mode;
    pslot->rsc[RSC_PROGRAM].uilock = -1;
    pslot->rsc[RSC_PROGRAM].slot = pslot;
    pslot->rsc[RSC_EEPROM].name = FN_EEPROM;
    pslot->rsc[RSC_EEPROM].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_EEPROM].bkey = 0;
    pslot->rsc[RSC_EEPROM].pgscb = cb_program_mode;
    pslot->rsc[RSC_EEPROM].uilock = -1;
    pslot->rsc[RSC_EEPROM].slot = pslot;

    // data memory resources
    pslot->rsc[RSC_RAM].name = FN_RAM;
    pslot->rsc[RSC_RAM].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_RAM].bkey = 0;
    pslot->rsc[RSC_RAM].pgscb = cb_data_mode;
    pslot->rsc[RSC_RAM].uilock = -1;
    pslot->rsc[RSC_RAM].slot = pslot;
    pslot->rsc[RSC_FIFO].name = FN_FIFO;
    pslot->rsc[RSC_FIFO].flags = IS_WRITABLE;
    pslot->rsc[RSC_FIFO].bkey = 0;
    pslot->rsc[RSC_FIFO].pgscb = cb_data_mode;
    pslot->rsc[RSC_FIFO].uilock = -1;
    pslot->rsc[RSC_FIFO].slot = pslot;
    pslot->rsc[RSC_REG].name = FN_REG;
    pslot->rsc[RSC_REG].flags = IS_READABLE | IS_WRITABLE;
    pslot->rsc[RSC_REG].bkey = 0;
    pslot->rsc[RSC_REG].pgscb = cb_data_mode;
    pslot->rsc[RSC_REG].uilock = -1;
    pslot->rsc[RSC_REG].slot = pslot;

    pslot->name = "avr";
    pslot->desc = "an AVR peripheral";
    pslot->help = README;

    // initialize the AVR SPI config to 100000MHz clock active low CS
    pctx->nbxfer = 0;
    pctx->spiport.clksrc = CLK_100K;
    pctx->spiport.csmode = CS_MODE_AL;
    if (send_spi(pctx) != 0) {
        edlog("Unable to send config to AVR\n");
        return(-1);
    }

    return (0);
}


/**************************************************************
 * Handle incoming packets from the peripheral.
 * Check for unexpected packets, discard write response packet,
 * send read response packet data to UI.
 **************************************************************/
static void packet_hdlr(
    SLOT   *pslot,     // handle for our slot's internal info
    DP_PKT *pkt,       // the received packet
    int     len)       // number of bytes in the received packet
{
    // data private to the peripheral
    AVRDEV *pctx = (AVRDEV *)(pslot->priv);
    RSC    *prsc = &(pslot->rsc[RSC_DATA]);

    // Packets are either a write reply or an auto send SPI reply.
    // The auto-send packet should have a count two (for the 2 config bytes)
    // and the number of bytes in the SPI packet (nbxfer).
    if (!(( //autosend packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) == DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 16))
          ||    ( // write response packet for mosi data packet
           ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_COUNT) && (pkt->count == (1 + pctx->nbxfer)))
          ||     ( // write response packet for config
           (((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) &&
            (pkt->reg == QCSPI_REG_MODE) && (pkt->count == 1))) ) )
    {
        // unknown packet
        edlog("invalid espi packet from board to host");
        return;
    }

    // Return if just the write reply
    if ((pkt->cmd & DP_CMD_AUTO_MASK) != DP_CMD_AUTO_DATA) {
        return;
    }

    // task state machines
    switch (pctx->taskId) {
        case TASK_SIGNATURE: {
            switch (pctx->taskState) {
                case 0:
                    // respond to the initial program enable instruction
                    if (pkt->data[REPLY_DATA_BYTE2] != 0x53) {
                        edlog("Unable to detect AVR.  Is the programming plug installed?\n");
                        prsc->uilock = -1;
                        return;
                    }

                    // get sig byte 0 -- use the unmodified general get sit byte instr
                    pctx->instruction = InstructionSet[OP_READ_SIG_BYTE];
                    if (send_instruction(pctx, pctx->instruction) != 0) {
                        edlog("Unable to send instruction to AVR\n");
                        prsc->uilock = -1;
                        return;
                    }

                    // go to sig read state
                    pctx->count = 0;
                    pctx->taskState = 1;
                    break;

                case 1:
                    // store next sig byte
                    pctx->signature[pctx->count] = pkt->data[REPLY_DATA_BYTE3];

                    // get next sig byte
                    pctx->count++;
                    if (pctx->count < SIGNATURE_LEN) {
                        // modify the general get sig byte instr
                        pctx->instruction.opnd2 = pctx->count;
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR\n");
                            prsc->uilock = -1;
                            return;
                        }
                        break;
                    }

                default:
                {
                    // return the signature to UI
                    return_ui(pctx->signature, 3, prsc);

                    // restore peripheral configuration and UI
                    Restore(prsc, pctx);
                    break;
                }
            }
            break;
        }

        // flash the AVR program memory
        case TASK_PROGRAM_SET: {
            switch (pctx->taskState) {
                // verify AVR can be detected and erase the program memory
                case 0:
                    // respond to the initial program enable instruction
                    if (pkt->data[REPLY_DATA_BYTE2] != 0x53) {
                        edlog("Unable to detect AVR.  Is the programming plug installed?\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    printf("\nprogramming AVR");

                    // send the erase instruction
                    if (send_instruction(pctx, InstructionSet[OP_ERASE]) != 0) {
                        edlog("Unable to send instruction to AVR\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    // delay as per the AVR spec to allow the erase to be performed
                    usleep(10000);

                    // init the memory page and byte address for programming
                    pctx->page = 0;
                    pctx->pageAddr = 0;

                    // change to programming state
                    pctx->taskState = 1;
                    break;

                // program the AVR page by page
                case 1:
                    if (pctx->pageAddr < PGSZ) {
                        if ((pctx->pageAddr & 0x0001) == 0) {
                            // even counts
                            pctx->instruction = InstructionSet[OP_LOAD_PMEM_PG_LO_BYTE];
                        }
                        else {
                            // odd counts
                            pctx->instruction = InstructionSet[OP_LOAD_PMEM_PG_HI_BYTE];
                        }

                        // load the LSB of the _word_ address (pageAddr/2) and next data byte
                        pctx->instruction.opnd2 = pctx->pageAddr >> 1;
                        pctx->instruction.opnd3 = *(pctx->pbuf + pctx->pageAddr + (pctx->page * PGSZ));
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR\n");
                            Restore(prsc, pctx);
                            break;
                        }
                        pctx->pageAddr++;
                    }
                    else {
                        // set the MSB and LSB of the address in opnd1 and opnd2 respectively
                        pctx->instruction = InstructionSet[OP_WRITE_PMEM_PG];
                        pctx->instruction.opnd1 = (pctx->page * PGSZ) >> 9;
                        pctx->instruction.opnd2 = (pctx->page * PGSZ >> 1) & 0x00ff;
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR\n");
                            Restore(prsc, pctx);
                            break;
                        }
                        printf(".");
                        fflush(stdout);

                        // delay as per the AVR spec to allow the page to be written
                        usleep(2600);

                        // increment page
                        pctx->page++;
                        if (pctx->page <= (pctx->imsz / PGSZ)) {
                            // program next page
                            pctx->pageAddr = 0;
                        }
                        else {
                            // last page programmed so change to verification state
                            pctx->taskState = 2;
                        }
                    }
                    break;


                case 2:
                    // read the first byte to kick off program verification
                    printf("\nverifying program");
                    pctx->instruction = InstructionSet[OP_READ_PMEM_PG_LO_BYTE];
                    if (send_instruction(pctx, pctx->instruction) != 0) {
                        edlog("Unable to send instruction to AVR\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    // init verification values
                    pctx->count = 0;
                    pctx->page = 0;
                    pctx->pageAddr = 0;

                    // change to verification state
                    pctx->taskState++;
                    break;

                case 3:
                    // verify current program byte
                    if (pkt->data[REPLY_DATA_BYTE3] != pctx->pbuf[pctx->count]) {
                        edlog("Unable to verify AVR program.  Is the programming plug installed?\n");
                        // uncomment this line for detailed error message
                        //printf("\n  at page %02x addr %02x: image value: %02x AVR value: %02x\n",
                        //    (pctx->count / PGSZ), (pctx->count % PGSZ), pctx->pbuf[pctx->count], pkt->data[REPLY_DATA_BYTE3]);
                        Restore(prsc, pctx);
                        break;
                    }
                    if (pctx->count % PGSZ == 0)
                        printf(".");
                    fflush(stdout);

                    // read next byte until all bytes have been verified then fall thru to final state
                    pctx->count++;
                    if (pctx->count < pctx->imsz) {
                        if ((pctx->count & 0x0001) == 0) {
                            // even counts
                            pctx->instruction = InstructionSet[OP_READ_PMEM_PG_LO_BYTE];
                        }
                        else {
                            // odd counts
                            pctx->instruction = InstructionSet[OP_READ_PMEM_PG_HI_BYTE];
                        }

                        // load the MSB and LSB of the word address
                        pctx->instruction.opnd1 = (pctx->count >> 9) & 0x00ff;
                        pctx->instruction.opnd2 = (pctx->count >> 1) & 0x00ff;
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR\n");
                            Restore(prsc, pctx);
                            break;
                        }

                        // continue with verification
                        break;
                    }

                default:
                    printf("\n...programming complete\n");

                    // free the buffers and restore the peripheral to its original state
                    free(pctx->pbuf);
                    Restore(prsc, pctx);
                    break;
            }
            break;
        }

        // dump AVR program memory
        case TASK_PROGRAM_GET: {
            switch (pctx->taskState) {
                case 0:
                    // respond to the initial program enable instruction
                    if (pkt->data[REPLY_DATA_BYTE2] != 0x53) {
                        edlog("Unable to detect AVR.  Is the programming plug installed?\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    printf("writing program memory image to file %s", pctx->filename);

                    // read the first byte to kick off the dump process
                    pctx->instruction = InstructionSet[OP_READ_PMEM_PG_LO_BYTE];
                    if (send_instruction(pctx, pctx->instruction) != 0) {
                        edlog("Unable to send instruction to AVR\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    // change to read state
                    pctx->count = 0;
                    pctx->imsz = 0;
                    pctx->taskState = 1;
                    break;

                case 1:
                    // write the current byte to the buffer
                    if (pctx->count % PGSZ == 0) {
                        printf(".");
                        fflush(stdout);
                    }
                    pctx->pbuf[pctx->imsz++] = pkt->data[REPLY_DATA_BYTE3];

                    // read the next byte of the image
                    pctx->count++;
                    if (pctx->count < PMEMSZ) {
                        if ((pctx->count & 0x0001) == 0) {
                            // even counts
                            pctx->instruction = InstructionSet[OP_READ_PMEM_PG_LO_BYTE];
                        }
                        else {
                            // odd counts
                            pctx->instruction = InstructionSet[OP_READ_PMEM_PG_HI_BYTE];
                        }

                        // load the MSB and LSB of the word address to read
                        pctx->instruction.opnd1 = (pctx->count >> 9) & 0x00ff;
                        pctx->instruction.opnd2 = (pctx->count >> 1) & 0x00ff;
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR\n");
                            Restore(prsc, pctx);
                            break;
                        }
                        break;
                    }

                default:  {
                    int i, fileSize;

                    // strip off all trailing 0xff bytes then all trailing 0x00 bytes
                    for (i = PMEMSZ - 1; i >= 0 ; i--) {
                        if (pctx->pbuf[i] != 0xff)
                        {
                            break;
                        }
                    }
                    pctx->imsz = i + 1;
                    for (i = pctx->imsz - 1; i >= 0 ; i--) {
                        if (pctx->pbuf[i] != 0x00)
                        {
                            break;
                        }
                    }
                    pctx->imsz = i + 1;

                    // write the image to the file
                    fileSize = put_pgm_image(pctx->pbuf, pctx->imsz, pctx->filename);

                    // send resulting file size to UI (-1 if any errors)
                    char ob[MAX_LINE_LEN] = {0};
                    sprintf(ob,"%04x\n", fileSize);
                    send_ui(ob, 5, prsc->uilock);
                    prompt(prsc->uilock);

                    // free the buffers and restore the peripheral to its original state
                    free(pctx->pbuf);
                    Restore(prsc, pctx);

                    printf("\n...image write complete\n");
                    break;
                }
            }
            break;
        }

        // load AVR EEPROM
        case TASK_EEPROM_SET: {
            switch (pctx->taskState) {
                // verify AVR can be detected and begin the EEPROM loading process
                case 0:
                    // respond to the initial program enable instruction
                    if (pkt->data[REPLY_DATA_BYTE2] != 0x53) {
                        edlog("Unable to detect AVR.  Is the programming plug installed?\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    printf("\nloading %d bytes into EEPROM beginning at address 0x%04x", pctx->imsz, pctx->eepromAddr);

                    // change to EEPROM read state
                    pctx->count = 0;
                    pctx->taskState = 1;

                case 1:
                    if (pctx->count < pctx->imsz) {
                        // read the next byte from the EEPROM to determine if a modification is necessary
                        pctx->instruction = InstructionSet[OP_READ_EEPROM];
                        pctx->instruction.opnd1 = ((pctx->eepromAddr + pctx->count) >> 8) & 0x00ff;
                        pctx->instruction.opnd2 = (pctx->eepromAddr + pctx->count) & 0x00ff;
                        pctx->instruction.opnd3 = 0;
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR.  Is the programming plug installed?\n");
                            Restore(prsc, pctx);
                            break;
                        }

                        // change to modify state
                        pctx->taskState = 2;
                        break;
                    }
 
                case 2:
                    if (pctx->count < pctx->imsz) {
                        // modify the next byte to the EEPROM if it is different from what is already there
                        if (pkt->data[REPLY_DATA_BYTE3] != pctx->pbuf[pctx->count])
                        {
                            pctx->instruction = InstructionSet[OP_WRITE_EEPROM];
                            pctx->instruction.opnd1 = ((pctx->eepromAddr + pctx->count) >> 8) & 0x00ff;
                            pctx->instruction.opnd2 = (pctx->eepromAddr + pctx->count) & 0x00ff;
                            pctx->instruction.opnd3 = pctx->pbuf[pctx->count];
                            if (send_instruction(pctx, pctx->instruction) != 0) {
                                edlog("Unable to send instruction to AVR\n");
                                Restore(prsc, pctx);
                                break;
                            }

                            // delay as per the AVR spec to allow the byte to be written
                            usleep(3600);

                            // change to read-for-verify state
                            pctx->taskState = 3;
                            break;
                        }
                        else {
                            // no need to make the modification so change to next read state
                            pctx->taskState = 1;
                            pctx->count++;
                        }
                    }

                case 3:
                    if (pctx->count < pctx->imsz) {
                        // read the next byte from the EEPROM for verification
                        pctx->instruction = InstructionSet[OP_READ_EEPROM];
                        pctx->instruction.opnd1 = ((pctx->eepromAddr + pctx->count) >> 8) & 0x00ff;
                        pctx->instruction.opnd2 = (pctx->eepromAddr + pctx->count) & 0x00ff;
                        pctx->instruction.opnd3 = 0;
                        if (send_instruction(pctx, pctx->instruction) != 0) {
                            edlog("Unable to send instruction to AVR.  Is the programming plug installed?\n");
                            Restore(prsc, pctx);
                            break;
                        }

                        // change to verify state
                        pctx->taskState = 4;
                        break;
                    }
 
                case 4:
                    if (pctx->count < pctx->imsz) {
                        // verify current program byte
                        if (pkt->data[REPLY_DATA_BYTE3] != pctx->pbuf[pctx->count]) {
                            edlog("Unable to verify EEPROM data.  Is the programming plug installed?\n");
                            // uncomment for more error detail
                            //printf("at addr %02x: data value: %02x EPROM value: %02x\n",
                            //    pctx->eepromAddr + pctx->count, pctx->pbuf[pctx->count], pkt->data[REPLY_DATA_BYTE3]);
                            Restore(prsc, pctx);
                            break;
                        }
                        printf(".");
                        fflush(stdout);

                        // read the next byte from the EEPROM to determine if a modification is necessary
                        pctx->count++;
                        if (pctx->count < pctx->imsz) {
                            pctx->instruction = InstructionSet[OP_READ_EEPROM];
                            pctx->instruction.opnd1 = ((pctx->eepromAddr + pctx->count) >> 8) & 0x00ff;
                            pctx->instruction.opnd2 = (pctx->eepromAddr + pctx->count) & 0x00ff;
                            pctx->instruction.opnd3 = 0;
                            if (send_instruction(pctx, pctx->instruction) != 0) {
                                edlog("Unable to send instruction to AVR\n");
                                Restore(prsc, pctx);
                                break;
                            }
                            pctx->taskState = 2;
                            break;
                        }
                    }
 
                default:
                    // free the buffers and restore the peripheral to its original state
                    free(pctx->pbuf);
                    Restore(prsc, pctx);

                    printf("\n...EEPROM loading complete\n");
                    break;
            }
            break;
        }

        // dump AVR EEPROM
        case TASK_EEPROM_GET: {
            switch (pctx->taskState) {
                // verify AVR can be detected and begin the EEPROM loading process
                case 0:
                    // respond to the initial program enable instruction
                    if (pkt->data[REPLY_DATA_BYTE2] != 0x53) {
                        edlog("Unable to detect AVR.  Is the programming plug installed?\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    // read first byte to kick off the state machine
                    pctx->count = 0;
                    pctx->instruction = InstructionSet[OP_READ_EEPROM];
                    pctx->instruction.opnd1 = ((pctx->eepromAddr + pctx->count) >> 8) & 0x00ff;
                    pctx->instruction.opnd2 = (pctx->eepromAddr + pctx->count) & 0x00ff;
                    pctx->instruction.opnd3 = 0;
                    if (send_instruction(pctx, pctx->instruction) != 0) {
                        edlog("Unable to send instruction to AVR\n");
                        Restore(prsc, pctx);
                        break;
                    }

                    // change to store/read state
                    pctx->taskState = 1;

                    break;
 
                case 1:
                    if (pctx->count < pctx->imsz) {
                        // store current byte
                        pctx->pbuf[pctx->count] = pkt->data[REPLY_DATA_BYTE3];

                        // read next byte
                        pctx->count++;
                        if (pctx->count < pctx->imsz) {
                            pctx->instruction = InstructionSet[OP_READ_EEPROM];
                            pctx->instruction.opnd1 = ((pctx->eepromAddr + pctx->count) >> 8) & 0x00ff;
                            pctx->instruction.opnd2 = (pctx->eepromAddr + pctx->count) & 0x00ff;
                            pctx->instruction.opnd3 = 0;
                            if (send_instruction(pctx, pctx->instruction) != 0) {
                                edlog("Unable to send instruction to AVR\n");
                                Restore(prsc, pctx);
                                break;
                            }

                            // stay in this state until all of the EEPROM bytes have been written
                            break;
                        }
                    }

                default:
                {
                    // return EEPROM bytes to UI
                    return_ui(pctx->pbuf, pctx->imsz, prsc);

                    // free the buffers and restore the peripheral to its original state
                    free(pctx->pbuf);
                    Restore(prsc, pctx);

                    break;
                }
            }
            break;
        }

        case TASK_DATA_GET:
        {
            // return the host register values:
            //   starting at offset 2 in the packet's data field
            //   the number of values is specified in the context's count member
            //return_ui(&pkt->data[REPLY_DATA_BYTE2], pctx->count, prsc);
            return_ui(&pkt->data[REPLY_DATA_BYTE3], pctx->count, prsc);

            // Response sent so clear the UI lock
            prsc->uilock = -1;
            break;
        }

        default:
        {
            // return data to UI -- skip the first 2 config values in the packet
            // and instead pass a pointer to the first _data_ byte in the packet
            // as the data buffer address
            return_ui(&pkt->data[REPLY_DATA_BYTE0], pctx->nbxfer, prsc);

            // Response sent so clear the UI lock
            prsc->uilock = -1;
            break;
        }
    }

    // response occurred so clear timer
    del_timer(pctx->ptimer);
    pctx->ptimer = 0;

    return;
}


// restore the original spiport config
static void Restore(RSC* prsc, AVRDEV *pctx)
{
    // a zero nbxfer tells send_spi() to set the config to what's in spiport
    pctx->nbxfer = 0;
    pctx->spiport.clksrc = pctx->origSpiport.clksrc;
    pctx->spiport.csmode = pctx->origSpiport.csmode;
    if (send_spi(pctx) != 0) {
        edlog("Unable to send config to AVR\n");
    }

    // clear the UI lock
    prsc->uilock = -1;
}


/**************************************************************
 * Callback used for all tasks that require the AVR to be put
 * into program mode.
 * Put the AVR into program enabled mode to kick off the state
 * machine for the resource.
 **************************************************************/
static void cb_program_mode(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    char *pbyte;
    int tmp;


    AVRDEV *pctx = pslot->priv;
    pctx->pSlot = pslot;

    // init the task state machine
    pctx->taskState = 0;

    if (rscid == RSC_PROGRAM) {
        // get the name of the file to use
        pbyte = strtok(val, ", ");
        strcpy(pctx->filename, pbyte);

        // allocate buffer to hold the image
        pctx->pbuf = (unsigned char *)malloc(PMEMSZ);
        if (pctx->pbuf == NULL) {
            // fatal error -- log and exit
            edlog("memory allocation error...exiting");
            exit(-1);
        }

        // define the task
        if(cmd == EDSET) {
            // flash the image from the given file to program memory
            pctx->imsz = get_pgm_image(pctx->pbuf, SZ_32K, pctx->filename);
            if (pctx->imsz == 0)
            {
                // simply return since the program function prints out error messages
                return;
            }
            pctx->taskId = TASK_PROGRAM_SET;
        }
        else {
            // write the image from program memory to the given file
            pctx->taskId = TASK_PROGRAM_GET;
        }
    }
    else if (rscid == RSC_EEPROM) {
        // parse the EEPROM beginning address from the UI -- the address value is guaranteed to be present
        pbyte = strtok(val, ", ");
        sscanf(pbyte, "%x", &pctx->eepromAddr);
        if (0 > pctx->eepromAddr || pctx->eepromAddr > 0x1ff) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }

        // allocate buffer to hold the bytes to load
        pctx->pbuf = (unsigned char *)malloc(EEPROMSZ);
        if (pctx->pbuf == NULL) {
            // fatal error -- log and exit
            edlog("memory allocation error...exiting");
            exit(-1);
        }

        // define the task
        if(cmd == EDSET) {
            // parse the data values into the transfer buffer
            pctx->imsz = 0;
            pbyte = strtok((char *) 0, ", ");
            while (pbyte) {
                sscanf(pbyte, "%x", &tmp);
                pctx->pbuf[pctx->imsz] = (unsigned char) (tmp & 0x00ff);
                pbyte = strtok((char *) 0, ", ");
                pctx->imsz++;
                if (pctx->imsz == (QCSPI_NDATA_BYTE - 2))
                    break;
            }

            // check byte quantity 1..400 and ensure no data will be written past the end of EEPROM
            if (1 > pctx->imsz || pctx->imsz > 0x200 || (pctx->eepromAddr + (pctx->imsz - 1)) > 0x1ff) {
                *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                return;
            }

            // load the EEPROM with data
            pctx->taskId = TASK_EEPROM_SET;
        }
        else {
            // parse the number of bytes of EEPROM to dump
            pbyte = strtok((char *) 0, ", ");
            if (pbyte) {
                sscanf(pbyte, "%x", &pctx->imsz);
            }
            else {
                *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
                return;
            }

            // dump the EEPROM data
            pctx->taskId = TASK_EEPROM_GET;
        }
    }
    else {
        // parse any numeric values sent from UI
        pctx->nbxfer = 0;
        if (val) {
            pbyte = strtok(val, ", ");
            while (pbyte) {
                sscanf(pbyte, "%x", &tmp);
                pctx->bxfer[pctx->nbxfer] = (unsigned char) (tmp & 0x00ff);
                pbyte = strtok((char *) 0, ", ");
                pctx->nbxfer++;
                if (pctx->nbxfer == (QCSPI_NDATA_BYTE - 2))
                    break;
            }
        }

        // define the task
        switch (rscid) {
            case RSC_SIGNATURE:
                pctx->taskId = TASK_SIGNATURE;
                break;
            default:
                pctx->taskId = TASK_DEFAULT;
                break;
        }
    }

    // save the original spiport config
    pctx->origSpiport.clksrc = pctx->spiport.clksrc;
    pctx->origSpiport.csmode = pctx->spiport.csmode;

    // force a pulse on CS to enable programming mode
    pctx->nbxfer = 0;
    pctx->spiport.clksrc  = CLK_100K;
    pctx->spiport.csmode  = CS_MODE_FH;
    if (send_spi(pctx) != 0) {
        // print FPGA write error and return
        *plen = snprintf(buf, *plen, E_WRFPGA);
        return;
    }
    pctx->spiport.csmode  = CS_MODE_FL;
    if (send_spi(pctx) != 0) {
        // print FPGA write error and return
        *plen = snprintf(buf, *plen, E_WRFPGA);
        return;
    }

    // send the initial program enable instruction
    if (send_instruction(pctx, InstructionSet[OP_PROGRAM_ENABLE]) != 0) {
        // print FPGA write error and return
        *plen = snprintf(buf, *plen, E_WRFPGA);
        return;
    }

    // lock this resource to the UI session cn
    pslot->rsc[RSC_DATA].uilock = (char) cn;

    // Nothing to send back to the user
    *plen = 0;

    return;
}


/**************************************************************
 * Callback used to handle data memory access
 *
 * This function builds an SPI transaction as follows:
 *   buffer[0]:         operation (vram/reg | rd/wr | autoinc)
 *   buffer[1]:         starting index
 *   buffer[2]:         number of bytes (read only)
 *   buffer[2..2+n]:    data bytes (write only)
 *
 *   buffer length:     number of command line args + 1 for the op
 *
 **************************************************************/
static void cb_data_mode(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    AVRDEV *pctx = pslot->priv;
    unsigned char cmdLineArgv[QCSPI_NDATA_BYTE - 2];
    int i, cmdLineArgc, dataQty, regIdxMin, regIdxMax;

    // init the state machine
    pctx->taskId = (cmd == EDSET) ? TASK_DEFAULT : TASK_DATA_GET;
    pctx->taskState = 0;

    // parse values from the UI into the command line arg list up to max bytes
    if ((parse_ui(val, cmdLineArgv, &cmdLineArgc) != 0) || (cmdLineArgc < 2)) {
        *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
        return;
    }

    // set index range and transaction operation
    if (rscid == RSC_RAM || rscid == RSC_FIFO) {
        regIdxMin = 0;
        regIdxMax = 63;
        pctx->bxfer[0] = OP_MEM;
        if (rscid == RSC_RAM) {
            pctx->bxfer[0] |= OP_AUTOINC;
        }
    }
    else {
        regIdxMin = 0x23;
        regIdxMax = 0xc6;
        pctx->bxfer[0] = OP_REG | OP_AUTOINC;
    }
    if (regIdxMin > REG_INDEX || REG_INDEX > regIdxMax) {
        *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
        return;
    }

    // write a set of values to register(s)
    if(cmd == EDSET) {
        // the number of bytes to write is the number of data bytes in the command line
        dataQty = cmdLineArgc - 1;

        // ensure that data quantity is 1..(QCSPI_NDATA_BYTE - 2)
        // and no resulting index can be greater than regIdxMax
        if ((1 > dataQty || dataQty > (QCSPI_NDATA_BYTE - 2)) ||
            ((REG_INDEX + (dataQty - 1)) > regIdxMax))
        {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }

        // build the host register write request: write op, start index, data qty, data...
        pctx->bxfer[0] |= OP_WR;
        pctx->bxfer[1] = REG_INDEX;
        for (i = 0; i < dataQty; i++) {
            pctx->bxfer[i+2] = DATA_VAL(i);
        }

        // transfer buffer length is data qty + 2 (opcode, index)
        pctx->nbxfer = dataQty + 2;
    }

    // read a set of values from consecutive host registers
    else {
        // the number of bytes to read is specified as the second command line arg
        dataQty = cmdLineArgv[1];
        pctx->count = dataQty;

        // ensure that data quantity is 1..(QCSPI_NDATA_BYTE - 2)
        // and no resulting index can be greater than regIdxMax
        if ((1 > dataQty || dataQty > (QCSPI_NDATA_BYTE - 2)) ||
            ((REG_INDEX + (dataQty - 1)) > regIdxMax))
        {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }

        // build the host register read request: read op, start index
        pctx->bxfer[0] |= OP_RD;
        pctx->bxfer[1] = REG_INDEX;

        // transfer buffer length is data qty + 2 (opcode, index)
        pctx->nbxfer = dataQty + 2;

        // lock this resource to the UI session cn
        pslot->rsc[RSC_DATA].uilock = (char) cn;

        // Nothing to send back to the user
        *plen = 0;
    }

    // send to SPI
    if (send_spi(pctx) != 0) {
        *plen = snprintf(buf, *plen, E_WRFPGA);
        return;
    }

    // Start timer to look for a read response.
    if (pctx->ptimer == 0)
        pctx->ptimer = add_timer(ED_ONESHOT, 100, no_ack, (void *) pctx);

    return;
}


/**************************************************************
 * Callback used to handle data resource from UI.
 * Read dpget parameters and send them to the peripheral.
 * Response packets will be handled in packet_hdlr().
 **************************************************************/
static void cb_data(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    char      *pbyte;
    int        tmp;

    int txret;

    if(cmd == EDGET) {
        AVRDEV *pctx = pslot->priv;

        // clear the state machine
        pctx->taskId = TASK_DEFAULT;
        pctx->taskState = 0;

        // Get the bytes to send
        pctx->nbxfer = 0;
        pctx->pSlot = pslot;
        pbyte = strtok(val, ", ");
        while (pbyte) {
            sscanf(pbyte, "%x", &tmp);
            pctx->bxfer[pctx->nbxfer] = (unsigned char) (tmp & 0x00ff);
            pbyte = strtok((char *) 0, ", ");   // commas or spaces accepted
            pctx->nbxfer++;
            if (pctx->nbxfer == (QCSPI_NDATA_BYTE - 2))
                break;
        }

        if (pctx->nbxfer != 0) {
            txret = send_spi(pctx);
            if (txret != 0) {
                *plen = snprintf(buf, *plen, E_WRFPGA);
                // (errors are handled in calling routine)
                return;
            }

            // Start timer to look for a read response.
            if (pctx->ptimer == 0)
                pctx->ptimer = add_timer(ED_ONESHOT, 100, no_ack, (void *) pctx);

            // lock this resource to the UI session cn
            pslot->rsc[RSC_DATA].uilock = (char) cn;

            // Nothing to send back to the user
            *plen = 0;
        }
        else {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
    }

    return;
}


/**************************************************************
 * Callback used to handle config resource from UI.
 * Read dpset parameters and send them to the peripheral.
 * On dpget, return current configuration to UI.
 **************************************************************/
static void cb_config(
    int      cmd,      //==EDGET if a read, ==EDSET on write
    int      rscid,    // ID of resource being accessed
    char    *val,      // new value for the resource
    SLOT    *pslot,    // pointer to slot info.
    int      cn,       // Index into UI table for requesting conn
    int     *plen,     // size of buf on input, #char in buf on output
    char    *buf)
{
    SPIPORT *pSPIport;
    int      newclk = -1;
    int      newcsmode = -1;
    char     ibuf[MAX_LINE_LEN];
    int      outlen = 0;

    RSC *prsc = &(pslot->rsc[RSC_DATA]);
    AVRDEV *pctx = pslot->priv;
    pSPIport = (SPIPORT *) &(pctx->spiport);

    // clear the state machine
    pctx->taskId = TASK_DEFAULT;
    pctx->taskState = 0;

    if (cmd == EDSET) {
        if (sscanf(val, "%d %s\n", &newclk, ibuf) != 2) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }
        if (!strncmp(ibuf, "al", 2))
            newcsmode = CS_MODE_AL;
        else if (!strncmp(ibuf, "ah", 2))
            newcsmode = CS_MODE_AH;
        else if (!strncmp(ibuf, "fl", 2))
            newcsmode = CS_MODE_FL;
        else if (!strncmp(ibuf, "fh", 2))
            newcsmode = CS_MODE_FH;
        // Sanity check on the inputs
        if ((newclk < 5000) ||
            (newcsmode < 0) || (newcsmode > 3)) {
            *plen = snprintf(buf, *plen,  E_BDVAL, pslot->rsc[rscid].name);
            return;
        }

        if (newclk >= 2000000)
            newclk = CLK_2M;
        else if (newclk >= 1000000)
            newclk = CLK_1M;
        else if (newclk >= 500000)
            newclk = CLK_500K;
        else
            newclk = CLK_100K;

        // Save new user defined configuration
        pSPIport->csmode  = newcsmode;
        pSPIport->clksrc  = newclk;

        // Send just the config so set nbxfer to zero and then send the packet
        pctx->nbxfer = 0;

        int txret = send_spi(pctx);

        if (txret != 0) {
            *plen = snprintf(buf, *plen, E_WRFPGA);
            // (errors are handled in calling routine)
            return;
        }
    }
    else {
        // write out the current configuration
        if (pSPIport->clksrc == CLK_2M)
            newclk = 2000000;
        else if (pSPIport->clksrc == CLK_1M)
            newclk = 1000000;
        else if (pSPIport->clksrc == CLK_500K)
            newclk = 500000;
        else
            newclk = 100000;
        if (pSPIport->csmode == CS_MODE_AL)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d al\n", newclk);
        else if (pSPIport->csmode == CS_MODE_AH)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d ah\n", newclk);
        else if (pSPIport->csmode == CS_MODE_FL)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d fl\n", newclk);
        else if (pSPIport->csmode == CS_MODE_FH)
            outlen = snprintf(ibuf, MAX_LINE_LEN, "%d fh\n", newclk);

        prsc->uilock = (char) cn;
        send_ui(ibuf, outlen, prsc->uilock);
        prompt(prsc->uilock);
        prsc->uilock = -1;

    }

    return;
}


/**************************************************************
 * Function to abstract the sending of an AVR programming
 * instruction.
 * Returns 0 on success, or negative tx_pkt() error code.
 **************************************************************/
int send_instruction(AVRDEV *pctx, INSTR instruction)
{
    int txret;

    // load the 4 instruction bytes into the SPI transfer buffer
    pctx->bxfer[0] = instruction.opcode;
    pctx->bxfer[1] = instruction.opnd1;
    pctx->bxfer[2] = instruction.opnd2;
    pctx->bxfer[3] = instruction.opnd3;
    pctx->nbxfer = 4;

    // perform the transaction
    txret = send_spi(pctx);
    if (txret == 0) {
        // Start timer to look for a read response.
        if (pctx->ptimer == 0) {
            pctx->ptimer = add_timer(ED_ONESHOT, 100, no_ack, (void *) pctx);
        }
    }

    return txret;
}


/**************************************************************
 * Function to handle actual SPI data transfer to peripheral.
 * Returns 0 on success, or negative tx_pkt() error code.
 **************************************************************/
static int send_spi(
    AVRDEV *pctx)    // This peripheral's context
{
    DP_PKT   pkt;
    SLOT    *pmyslot;  // Our slot info
    CORE    *pmycore;  // FPGA peripheral info
    int      txret;    // ==0 if the packet went out OK
    int      i;
    SPIPORT *pSPIport;

    pSPIport = &(pctx->spiport);
    pmyslot = pctx->pSlot;
    pmycore = pmyslot->pcore;

    // create a write packet to set the mode reg
    pkt.cmd = DP_CMD_OP_WRITE | DP_CMD_AUTOINC;
    pkt.core = pmycore->core_id;

    if (pctx->nbxfer == 0) {
        // send the clock source and SPI mode
        pkt.data[0] = (pSPIport->clksrc << 6) | (pSPIport->csmode << 2) ;
        pkt.reg = QCSPI_REG_MODE;
        pkt.count = 1;
    }
    else {
        pkt.reg = QCSPI_REG_COUNT;
        pkt.count = 1 + pctx->nbxfer;  // sending count plus all SPI pkt bytes
        pkt.data[0] = 1 + pctx->nbxfer;  // max RAM address in the peripheral

        // Copy the SPI packet to the DP packet data
        for (i = 0; i < pctx->nbxfer; i++)
            pkt.data[i + 1] = pctx->bxfer[i];
    }

    // try to send the packet.  Schedule a resend on tx failure
    txret = dpi_tx_pkt(pmycore, &pkt, 4 + pkt.count); // 4 header + data

    return txret;
}


/**************************************************************
 * noAck():  Wrote to the board but did not get a reply.  Handle
 * the timeout for this.
 **************************************************************/
static void no_ack(
    void     *timer,   // handle of the timer that expired
    AVRDEV *pctx)
{
    // Log the missing ack
    edlog(E_NOACK);

    return;
}


/*******************************************************************
 * get_pgm_image():  - Read the program image from a file.  Return the
 *      number of bytes in the image or a negative number on error.
 *******************************************************************/
int get_pgm_image(unsigned char *pbuf, int max, char *pgm_file)
{
    FILE *pFile;     // input file
    char  ln[MXLN];  // a line from the input file
    int   count;     // number of bytes in program image
    int   dcount;    // number of bytes in input line
    int   addr;      // where to place the data in pbuf
    int   type;      // record type
    int   lnln;      // line length
    int   chksum;    // check sum from input line
    int   sum;       // sum computed from input fields
    int   i;


    /*
     *                0 1 2 3 4 5 6 7 8 9 a b c d e f
     *  : 10 0000 00 0C94B2040C9459150C9484150C941F05 8F
     *  :100010000C94CF040C94CF040C94CF040C94CF0414

     *  start code:     ':'
     *  byte count:     10 == 16 bytes in line
     *  address:        0010 == 16 bit address
     *  record type:    00==data, 01==EOF
     *  data bytes:     count bytes of hex data
     *  checksum:       XX == 8 bit sum of all hex bytes
     *  minimum line length is 1+2+4+2+0+2+1(newline) = 12
     *  actual line length is 12+2 * byte_count
     */

    pFile = fopen(pgm_file, "r");
    if (NULL == pFile) {
        printf("Unable to open program image file: %s\n", pgm_file);
        return 0;
    }

    count = 0;
    while (NULL != fgets(ln, MXLN, pFile)) {
        lnln = strnlen(ln, MXLN);
        if ((':' != ln[0]) || (12 > lnln))    // valid line?
            continue;
        dcount = (16 * a2h(ln[1])) + a2h(ln[2]);
        sum = dcount;
        addr = (4096 * a2h(ln[3])) + (256 * a2h(ln[4])) + (16 * a2h(ln[5])) + a2h(ln[6]);
        sum += addr & 0x00ff;
        sum += (addr >> 8) & 0x00ff;
        type = (16 * a2h(ln[7])) + a2h(ln[8]);
        sum += type;

        /* Sanity check: addr + date length <= buffer size,
         * and actual line length (but line have have \n or CRLF*/
        if ((addr + dcount > max) ||
            (12 + (2 * dcount) > lnln)) {
            printf("Error in hex file format\n");
            exit(1);
        }

        /* Everything checks out.  Put data bytes in pbuf */
        for (i = 0; i < dcount ; i++) {
            // hex data starts at the ninth character in the line.
            *(pbuf + addr + i) = (16 * a2h(ln[(2 * i) + 9])) + a2h(ln[(2 * i) + 10]);
            sum += *(pbuf + addr + i);
        }
        count += dcount;

        /* verify line checksum */
        chksum = (16 * a2h(ln[9 + (2 * dcount)])) + a2h(ln[10 + (2 * dcount)]);
        sum = (0x0100 - sum) & 0x00ff;
        if (chksum != sum) {
            printf("Checksum error in hex file\n");
            fclose(pFile);
            exit(1);
        }

        /* return if line type is EOF */
        if (0x01 == type) {
            fclose(pFile);
            return count;
        }
    }

    fclose(pFile);

    return count;
}

/*******************************************************************
 * a2h(): convert one hex character to an integer.
 *******************************************************************/
int a2h(char digit)
{
    int out = 0;
    char hex;

    if ((digit >= '0') && (digit <= '9'))
        out = (int)(digit - '0');
    else {
        hex = tolower(digit);
        if ((hex >= 'a') && (hex <= 'f'))
            out = 10 + (int)(hex - 'a');
    }
    return(out);
}

/******************************************************************************
 * put_pgm_image():  - Write the program image to a file.  Return the
 *      number of bytes written or a negative number on error.
 *
 * Write the image as hex file records to the file
 *   hex format: :llaaaatt[dd...]cc
 *     ll    number of data bytes in the record
 *     aaaa  starting address of the data in the record
 *     tt    type of record: 00 - data record or 01 - end-of-file record
 *     dd    data byte
 *     cc    checksum = 2's complement of the sum of all fields mod 256
 *****************************************************************************/
int put_pgm_image(unsigned char *pbuf, int len, char *filename)
{
    FILE    *pFile;             // input file
    int      i, j;              // indices used to create the record
    int      pbufIdx;           // pbuf index
    int      fullRecordQty;     // number of records to write
    int      partialDataQty;    // number of bytes in last, partial record
    char     byteStr[3];        // ascii representation of a byte
    unsigned char recordLen;    // hex file record length field
    int      offset;            // hex file starting address field
    unsigned char recordType;   // hex file record type field
    char     dataStr[80];       // hex file ascii data field
    unsigned char checksum;     // hex file checksum field
    int      count;             // number of bytes written to the file

    // determine the number of records and the amount of bytes in the last, partial record
    fullRecordQty = len / RECORD_DATA_SIZE;
    partialDataQty = len % RECORD_DATA_SIZE;

    // open the file
    pFile = fopen(filename, "w");
    if (NULL == pFile) {
        printf("Unable to open program image file: %s\n", filename);
        return -1;
    }

    // write the data records
    pbufIdx = 0;
    count = 0;
    for (i = 0; i < fullRecordQty + 1; i++) {
        // set the record fields
        recordLen = (i < fullRecordQty) ? RECORD_DATA_SIZE : partialDataQty;
        offset = i * RECORD_DATA_SIZE;
        recordType = RECORD_TYPE_DATA;
        checksum = recordLen + (offset >> 8) + (offset & 0x00ff) + recordType;

        // build the data string
        dataStr[0] = '\0';
        for (j = 0; j < recordLen; j++)
        {
            // create a string of converted data
            sprintf(byteStr, "%02X", pbuf[pbufIdx]);
            strcat(dataStr, byteStr);
            checksum += pbuf[pbufIdx];
            pbufIdx++;
        }
        checksum = ~(checksum) + 1;

        // write the record to the file
        if (fprintf(pFile, ":%02X%04X%02X%s%02X\r\n", recordLen, offset, recordType, dataStr, checksum) == EOF) {
            edlog("Unable to write to file\n");
            fclose(pFile);
            return -1;
        }
        count += 1 + 2 + 4 + 2 + (2 * recordLen) + 2 + 2;
    }

    // write the end of file record
    if (fprintf(pFile, ":00000001FF\r\n") == EOF) {
        edlog("Unable to write to file\n");
        fclose(pFile);
        return -1;
    }
    count += 1 + 2 + 4 + 2 + 2 + 2;

    fclose(pFile);

    return count;
}

// parse a list of ASCII values into a buffer of integers and its length
int parse_ui(char* uiStrList, unsigned char buffer[], int* pLen)
{
    int retval = -1;

    if (uiStrList) {
        *pLen = 0;
        char *pbyte = strtok(uiStrList, ", ");
        while (pbyte) {
            sscanf(pbyte, "%hhx", &buffer[(*pLen)++]);
            pbyte = strtok((char *) 0, ", ");
        }
        retval = 0;
    }

    return retval;
}

static void return_ui(unsigned char valList[], int valListLen, RSC* prsc)
{
    char valStr[(MAX_LINE_LEN * 3) + 1] = {0};
    int valStrlen = 0;
    int i;

    // put the ascii representation of the values into the buffer and append a newline
    for(i = 0; i < valListLen; i++) {
        sprintf(&valStr[i * 3], "%02x ", valList[i]);
        valStrlen += 3;
    }
    sprintf(&valStr[i * 3], "\n");
    valStrlen += 1;

    // return value string back to the UI
    send_ui(valStr, valStrlen, prsc->uilock);
    prompt(prsc->uilock);
}

// end of avr.c
