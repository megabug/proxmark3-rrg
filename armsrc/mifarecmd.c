//-----------------------------------------------------------------------------
// Merlok - June 2011, 2012
// Gerhard de Koning Gans - May 2008
// Hagen Fritsch - June 2010
// Midnitesnake - Dec 2013
// Andy Davies  - Apr 2014
// Iceman - May 2014,2015,2016
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Routines to support ISO 14443 type A.
//-----------------------------------------------------------------------------

#include "mifarecmd.h"

#include "pmflash.h"
#include "proxmark3_arm.h"
#include "string.h"
#include "mifareutil.h"
#include "protocols.h"
#include "parity.h"
#include "BigBuf.h"
#include "cmd.h"
#include "flashmem.h"
#include "fpgaloader.h"
#include "iso14443a.h"
#include "mifaredesfire.h"
#include "util.h"
#include "commonutil.h"
#include "crc16.h"
#include "dbprint.h"
#include "ticks.h"

#ifndef HARDNESTED_AUTHENTICATION_TIMEOUT
# define HARDNESTED_AUTHENTICATION_TIMEOUT  848     // card times out 1ms after wrong authentication (according to NXP documentation)
#endif
#ifndef HARDNESTED_PRE_AUTHENTICATION_LEADTIME
# define HARDNESTED_PRE_AUTHENTICATION_LEADTIME 400 // some (non standard) cards need a pause after select before they are ready for first authentication
#endif

// send an incomplete dummy response in order to trigger the card's authentication failure timeout
#ifndef CHK_TIMEOUT
# define CHK_TIMEOUT() { \
        ReaderTransmit(&dummy_answer, 1, NULL); \
        uint32_t timeout = GetCountSspClk() + HARDNESTED_AUTHENTICATION_TIMEOUT; \
        while (GetCountSspClk() < timeout) {}; \
    }
#endif

static uint8_t dummy_answer = 0;

//-----------------------------------------------------------------------------
// Select, Authenticate, Read a MIFARE tag.
// read block
//-----------------------------------------------------------------------------
void MifareReadBlock(uint8_t blockNo, uint8_t keyType, uint8_t *datain) {
    // params
    uint64_t ui64Key = 0;
    ui64Key = bytes_to_num(datain, 6);

    // variables
    uint8_t dataoutbuf[16] = {0x00};
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0, status = PM3_EOPABORTED;

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    while (true) {
        if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
            if (DBGLEVEL >= 1) Dbprintf("Can't select card");
            break;
        };

        if (mifare_classic_auth(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST)) {
            if (DBGLEVEL >= 1) Dbprintf("Auth error");
            break;
        };

        if (mifare_classic_readblock(pcs, cuid, blockNo, dataoutbuf)) {
            if (DBGLEVEL >= 1) Dbprintf("Read block error");
            break;
        };

        if (mifare_classic_halt(pcs, cuid)) {
            if (DBGLEVEL >= 1) Dbprintf("Halt error");
            break;
        };

        status = PM3_SUCCESS;
        break;
    }

    crypto1_destroy(pcs);

    if (DBGLEVEL >= 2) DbpString("READ BLOCK FINISHED");

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_READBL, status, dataoutbuf, 16);
    LED_B_OFF();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}

void MifareUC_Auth(uint8_t arg0, uint8_t *keybytes) {

    bool turnOffField = (arg0 == 1);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (!iso14443a_select_card(NULL, NULL, NULL, true, 0, true)) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    if (!mifare_ultra_auth(keybytes)) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Authentication failed");
        OnError(1);
        return;
    }

    if (turnOffField) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
    }
    reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
}

// Arg0 = BlockNo,
// Arg1 = UsePwd bool
// datain = PWD bytes,
void MifareUReadBlock(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    uint8_t blockNo = arg0;
    uint8_t dataout[16] = {0x00};
    bool useKey = (arg1 == 1); //UL_C
    bool usePwd = (arg1 == 2); //UL_EV1/NTAG

    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    int len = iso14443a_select_card(NULL, NULL, NULL, true, 0, true);
    if (!len) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Can't select card (RC:%02X)", len);
        OnError(1);
        return;
    }

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain, sizeof(key));

        if (!mifare_ultra_auth(key)) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain, 4);
        uint8_t pack[4] = {0, 0, 0, 0};
        if (!mifare_ul_ev1_auth(pwd, pack)) {
            OnError(1);
            return;
        }
    }

    if (mifare_ultra_readblock(blockNo, dataout)) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Read block error");
        OnError(2);
        return;
    }

    if (mifare_ultra_halt()) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Halt error");
        OnError(3);
        return;
    }

    reply_mix(CMD_ACK, 1, 0, 0, dataout, 16);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}

//-----------------------------------------------------------------------------
// Select, Authenticate, Read a MIFARE tag.
// read sector (data = 4 x 16 bytes = 64 bytes, or 16 x 16 bytes = 256 bytes)
//-----------------------------------------------------------------------------
void MifareReadSector(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    // params
    uint8_t sectorNo = arg0;
    uint8_t keyType = arg1;
    uint64_t ui64Key = 0;
    ui64Key = bytes_to_num(datain, 6);

    // variables
    uint8_t isOK = 0;
    uint8_t dataoutbuf[16 * 16];
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    isOK = 1;
    if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
        isOK = 0;
        if (DBGLEVEL >= 1) Dbprintf("Can't select card");
    }


    if (isOK && mifare_classic_auth(pcs, cuid, FirstBlockOfSector(sectorNo), keyType, ui64Key, AUTH_FIRST)) {
        isOK = 0;
        if (DBGLEVEL >= 1) Dbprintf("Auth error");
    }

    for (uint8_t blockNo = 0; isOK && blockNo < NumBlocksPerSector(sectorNo); blockNo++) {
        if (mifare_classic_readblock(pcs, cuid, FirstBlockOfSector(sectorNo) + blockNo, dataoutbuf + 16 * blockNo)) {
            isOK = 0;
            if (DBGLEVEL >= 1) Dbprintf("Read sector %2d block %2d error", sectorNo, blockNo);
            break;
        }
    }

    if (mifare_classic_halt(pcs, cuid)) {
        if (DBGLEVEL >= 1) Dbprintf("Halt error");
    }

    if (DBGLEVEL >= 2) DbpString("READ SECTOR FINISHED");

    crypto1_destroy(pcs);

    LED_B_ON();
    reply_old(CMD_ACK, isOK, 0, 0, dataoutbuf, 16 * NumBlocksPerSector(sectorNo));
    LED_B_OFF();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

// arg0 = blockNo (start)
// arg1 = Pages (number of blocks)
// arg2 = useKey
// datain = KEY bytes
void MifareUReadCard(uint8_t arg0, uint16_t arg1, uint8_t arg2, uint8_t *datain) {
    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    // params
    uint8_t blockNo = arg0;
    uint16_t blocks = arg1;
    bool useKey = (arg2 == 1); //UL_C
    bool usePwd = (arg2 == 2); //UL_EV1/NTAG
    uint32_t countblocks = 0;
    uint8_t *dataout = BigBuf_malloc(CARD_MEMORY_SIZE);
    if (dataout == NULL) {
        Dbprintf("out of memory");
        OnError(1);
        return;
    }

    int len = iso14443a_select_card(NULL, NULL, NULL, true, 0, true);
    if (!len) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Can't select card (RC:%d)", len);
        OnError(1);
        return;
    }

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain, sizeof(key));

        if (!mifare_ultra_auth(key)) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain, sizeof(pwd));
        uint8_t pack[4] = {0, 0, 0, 0};

        if (!mifare_ul_ev1_auth(pwd, pack)) {
            OnError(1);
            return;
        }
    }

    for (int i = 0; i < blocks; i++) {
        if ((i * 4) + 4 >= CARD_MEMORY_SIZE) {
            Dbprintf("Data exceeds buffer!!");
            break;
        }

        len = mifare_ultra_readblock(blockNo + i, dataout + 4 * i);

        if (len) {
            if (DBGLEVEL >= DBG_ERROR) Dbprintf("Read block %d error", i);
            // if no blocks read - error out
            if (i == 0) {
                OnError(2);
                return;
            } else {
                //stop at last successful read block and return what we got
                break;
            }
        } else {
            countblocks++;
        }
    }

    len = mifare_ultra_halt();
    if (len) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Halt error");
        OnError(3);
        return;
    }

    if (DBGLEVEL >= DBG_EXTENDED) Dbprintf("Blocks read %d", countblocks);

    countblocks *= 4;

    reply_mix(CMD_ACK, 1, countblocks, BigBuf_max_traceLen(), 0, 0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    BigBuf_free();
    set_tracing(false);
}

//-----------------------------------------------------------------------------
// Select, Authenticate, Write a MIFARE tag.
// read block
//-----------------------------------------------------------------------------
void MifareWriteBlock(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    // params
    uint8_t blockNo = arg0;
    uint8_t keyType = arg1;
    uint64_t ui64Key = 0;
    uint8_t blockdata[16] = {0x00};

    ui64Key = bytes_to_num(datain, 6);
    memcpy(blockdata, datain + 10, 16);

    // variables
    uint8_t isOK = 0;
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    while (true) {
        if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
            if (DBGLEVEL >= 1) Dbprintf("Can't select card");
            break;
        };

        if (mifare_classic_auth(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST)) {
            if (DBGLEVEL >= 1) Dbprintf("Auth error");
            break;
        };

        if (mifare_classic_writeblock(pcs, cuid, blockNo, blockdata)) {
            if (DBGLEVEL >= 1) Dbprintf("Write block error");
            break;
        };

        if (mifare_classic_halt(pcs, cuid)) {
            if (DBGLEVEL >= 1) Dbprintf("Halt error");
            break;
        };

        isOK = 1;
        break;
    }

    crypto1_destroy(pcs);

    if (DBGLEVEL >= 2) DbpString("WRITE BLOCK FINISHED");

    reply_mix(CMD_ACK, isOK, 0, 0, 0, 0);

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

/* // Command not needed but left for future testing
void MifareUWriteBlockCompat(uint8_t arg0, uint8_t *datain)
{
    uint8_t blockNo = arg0;
    uint8_t blockdata[16] = {0x00};

    memcpy(blockdata, datain, 16);

    uint8_t uid[10] = {0x00};

    LED_A_ON(); LED_B_OFF(); LED_C_OFF();

    clear_trace();
    set_tracing(true);
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    if(!iso14443a_select_card(uid, NULL, NULL, true, 0, true)) {
        if (DBGLEVEL >= 1)   Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    if(mifare_ultra_writeblock_compat(blockNo, blockdata)) {
        if (DBGLEVEL >= 1)   Dbprintf("Write block error");
        OnError(0);
        return; };

    if(mifare_ultra_halt()) {
        if (DBGLEVEL >= 1)   Dbprintf("Halt error");
        OnError(0);
        return;
    };

    if (DBGLEVEL >= 2)   DbpString("WRITE BLOCK FINISHED");

    reply_mix(CMD_ACK,1,0,0,0,0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}
*/

// Arg0   : Block to write to.
// Arg1   : 0 = use no authentication.
//          1 = use 0x1A authentication.
//          2 = use 0x1B authentication.
// datain : 4 first bytes is data to be written.
//        : 4/16 next bytes is authentication key.
void MifareUWriteBlock(uint8_t arg0, uint8_t arg1, uint8_t *datain) {
    uint8_t blockNo = arg0;
    bool useKey = (arg1 == 1); //UL_C
    bool usePwd = (arg1 == 2); //UL_EV1/NTAG
    uint8_t blockdata[4] = {0x00};

    memcpy(blockdata, datain, 4);

    LEDsoff();
    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (!iso14443a_select_card(NULL, NULL, NULL, true, 0, true)) {
        if (DBGLEVEL >= 1) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    // UL-C authentication
    if (useKey) {
        uint8_t key[16] = {0x00};
        memcpy(key, datain + 4, sizeof(key));

        if (!mifare_ultra_auth(key)) {
            OnError(1);
            return;
        }
    }

    // UL-EV1 / NTAG authentication
    if (usePwd) {
        uint8_t pwd[4] = {0x00};
        memcpy(pwd, datain + 4, 4);
        uint8_t pack[4] = {0, 0, 0, 0};
        if (!mifare_ul_ev1_auth(pwd, pack)) {
            OnError(1);
            return;
        }
    }

    if (mifare_ultra_writeblock(blockNo, blockdata)) {
        if (DBGLEVEL >= 1) Dbprintf("Write block error");
        OnError(0);
        return;
    };

    if (mifare_ultra_halt()) {
        if (DBGLEVEL >= 1) Dbprintf("Halt error");
        OnError(0);
        return;
    };

    if (DBGLEVEL >= 2) DbpString("WRITE BLOCK FINISHED");

    reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

void MifareUSetPwd(uint8_t arg0, uint8_t *datain) {

    uint8_t pwd[16] = {0x00};
    uint8_t blockdata[4] = {0x00};

    memcpy(pwd, datain, 16);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    if (!iso14443a_select_card(NULL, NULL, NULL, true, 0, true)) {
        if (DBGLEVEL >= 1) Dbprintf("Can't select card");
        OnError(0);
        return;
    };

    blockdata[0] = pwd[7];
    blockdata[1] = pwd[6];
    blockdata[2] = pwd[5];
    blockdata[3] = pwd[4];
    if (mifare_ultra_writeblock(44, blockdata)) {
        if (DBGLEVEL >= 1) Dbprintf("Write block error");
        OnError(44);
        return;
    };

    blockdata[0] = pwd[3];
    blockdata[1] = pwd[2];
    blockdata[2] = pwd[1];
    blockdata[3] = pwd[0];
    if (mifare_ultra_writeblock(45, blockdata)) {
        if (DBGLEVEL >= 1) Dbprintf("Write block error");
        OnError(45);
        return;
    };

    blockdata[0] = pwd[15];
    blockdata[1] = pwd[14];
    blockdata[2] = pwd[13];
    blockdata[3] = pwd[12];
    if (mifare_ultra_writeblock(46, blockdata)) {
        if (DBGLEVEL >= 1) Dbprintf("Write block error");
        OnError(46);
        return;
    };

    blockdata[0] = pwd[11];
    blockdata[1] = pwd[10];
    blockdata[2] = pwd[9];
    blockdata[3] = pwd[8];
    if (mifare_ultra_writeblock(47, blockdata)) {
        if (DBGLEVEL >= 1) Dbprintf("Write block error");
        OnError(47);
        return;
    };

    if (mifare_ultra_halt()) {
        if (DBGLEVEL >= 1) Dbprintf("Halt error");
        OnError(0);
        return;
    };

    reply_mix(CMD_ACK, 1, 0, 0, 0, 0);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

// Return 1 if the nonce is invalid else return 0
int valid_nonce(uint32_t Nt, uint32_t NtEnc, uint32_t Ks1, uint8_t *parity) {
    return ((oddparity8((Nt >> 24) & 0xFF) == ((parity[0]) ^ oddparity8((NtEnc >> 24) & 0xFF) ^ BIT(Ks1, 16))) & \
            (oddparity8((Nt >> 16) & 0xFF) == ((parity[1]) ^ oddparity8((NtEnc >> 16) & 0xFF) ^ BIT(Ks1, 8))) & \
            (oddparity8((Nt >> 8) & 0xFF) == ((parity[2]) ^ oddparity8((NtEnc >> 8) & 0xFF) ^ BIT(Ks1, 0)))) ? 1 : 0;
}

void MifareAcquireNonces(uint32_t arg0, uint32_t flags) {

    uint8_t uid[10] = {0x00};
    uint8_t answer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t par[1] = {0x00};
    uint8_t buf[PM3_CMD_DATA_SIZE] = {0x00};
    uint32_t cuid = 0;
    int16_t isOK = 0;
    uint16_t num_nonces = 0;
    uint8_t cascade_levels = 0;
    uint8_t blockNo = arg0 & 0xff;
    uint8_t keyType = (arg0 >> 8) & 0xff;
    bool initialize = flags & 0x0001;
    bool field_off = flags & 0x0004;
    bool have_uid = false;

    LED_A_ON();
    LED_C_OFF();

    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    if (initialize)
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    LED_C_ON();

    for (uint16_t i = 0; i <= PM3_CMD_DATA_SIZE - 4; i += 4) {

        // Test if the action was cancelled
        if (BUTTON_PRESS()) {
            isOK = 2;
            field_off = true;
            break;
        }

        if (!have_uid) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (!iso14443a_select_card(uid, &card_info, &cuid, true, 0, true)) {
                if (DBGLEVEL >= 1) Dbprintf("AcquireNonces: Can't select card (ALL)");
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (!iso14443a_fast_select_card(uid, cascade_levels)) {
                if (DBGLEVEL >= 1) Dbprintf("AcquireNonces: Can't select card (UID)");
                continue;
            }
        }

        // Transmit MIFARE_CLASSIC_AUTH
        uint8_t dcmd[4] = {0x60 + (keyType & 0x01), blockNo, 0x00, 0x00};
        AddCrc14A(dcmd, 2);
        ReaderTransmit(dcmd, sizeof(dcmd), NULL);
        int len = ReaderReceive(answer, par);

        // wait for the card to become ready again
        CHK_TIMEOUT();

        if (len != 4) {
            if (DBGLEVEL >= 2) Dbprintf("AcquireNonces: Auth1 error");
            continue;
        }

        num_nonces++;

        // Save the tag nonce (nt)
        buf[i]   = answer[0];
        buf[i + 1] = answer[1];
        buf[i + 2] = answer[2];
        buf[i + 3] = answer[3];
    }

    LED_C_OFF();
    LED_B_ON();
    reply_old(CMD_ACK, isOK, cuid, num_nonces - 1, buf, sizeof(buf));
    LED_B_OFF();

    if (DBGLEVEL >= 3) DbpString("AcquireNonces finished");

    if (field_off) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
        set_tracing(false);
    }
}

//-----------------------------------------------------------------------------
// acquire encrypted nonces in order to perform the attack described in
// Carlo Meijer, Roel Verdult, "Ciphertext-only Cryptanalysis on Hardened
// Mifare Classic Cards" in Proceedings of the 22nd ACM SIGSAC Conference on
// Computer and Communications Security, 2015
//-----------------------------------------------------------------------------
void MifareAcquireEncryptedNonces(uint32_t arg0, uint32_t arg1, uint32_t flags, uint8_t *datain) {

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    uint8_t uid[10] = {0x00};
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t par_enc[1] = {0x00};
    uint8_t buf[PM3_CMD_DATA_SIZE] = {0x00};

    uint64_t ui64Key = bytes_to_num(datain, 6);
    uint32_t cuid = 0;
    int16_t isOK = 0;
    uint16_t num_nonces = 0;
    uint8_t nt_par_enc = 0;
    uint8_t cascade_levels = 0;
    uint8_t blockNo = arg0 & 0xff;
    uint8_t keyType = (arg0 >> 8) & 0xff;
    uint8_t targetBlockNo = arg1 & 0xff;
    uint8_t targetKeyType = (arg1 >> 8) & 0xff;
    bool initialize = flags & 0x0001;
    bool slow = flags & 0x0002;
    bool field_off = flags & 0x0004;
    bool have_uid = false;

    LED_A_ON();
    LED_C_OFF();

    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(false);

    if (initialize)
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    LED_C_ON();

    for (uint16_t i = 0; i <= PM3_CMD_DATA_SIZE - 9;) {

        // Test if the action was cancelled
        if (BUTTON_PRESS()) {
            isOK = 2;
            field_off = true;
            break;
        }

        if (!have_uid) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (!iso14443a_select_card(uid, &card_info, &cuid, true, 0, true)) {
                if (DBGLEVEL >= 1) Dbprintf("AcquireNonces: Can't select card (ALL)");
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (!iso14443a_fast_select_card(uid, cascade_levels)) {
                if (DBGLEVEL >= 1) Dbprintf("AcquireNonces: Can't select card (UID)");
                continue;
            }
        }

        if (slow)
            SpinDelayUs(HARDNESTED_PRE_AUTHENTICATION_LEADTIME);

        uint32_t nt1;
        if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, NULL)) {
            if (DBGLEVEL >= 1) Dbprintf("AcquireNonces: Auth1 error");
            continue;
        }

        // nested authentication
        uint16_t len = mifare_sendcmd_short(pcs, AUTH_NESTED, 0x60 + (targetKeyType & 0x01), targetBlockNo, receivedAnswer, par_enc, NULL);

        // wait for the card to become ready again
        CHK_TIMEOUT();

        if (len != 4) {
            if (DBGLEVEL >= 1) Dbprintf("AcquireNonces: Auth2 error len=%d", len);
            continue;
        }

        num_nonces++;
        if (num_nonces % 2) {
            memcpy(buf + i, receivedAnswer, 4);
            nt_par_enc = par_enc[0] & 0xf0;
        } else {
            nt_par_enc |= par_enc[0]  >> 4;
            memcpy(buf + i + 4, receivedAnswer, 4);
            memcpy(buf + i + 8, &nt_par_enc, 1);
            i += 9;
        }
    }

    LED_C_OFF();
    crypto1_destroy(pcs);
    LED_B_ON();
    reply_old(CMD_ACK, isOK, cuid, num_nonces, buf, sizeof(buf));
    LED_B_OFF();

    if (DBGLEVEL >= 3) DbpString("AcquireEncryptedNonces finished");

    if (field_off) {
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        LEDsoff();
        set_tracing(false);
    }
}


//-----------------------------------------------------------------------------
// MIFARE nested authentication.
//
//-----------------------------------------------------------------------------
void MifareNested(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint8_t *datain) {
    // params
    uint8_t blockNo = arg0 & 0xff;
    uint8_t keyType = (arg0 >> 8) & 0xff;
    uint8_t targetBlockNo = arg1 & 0xff;
    uint8_t targetKeyType = (arg1 >> 8) & 0xff;
    // calibrate = arg2
    uint64_t ui64Key = 0;

    ui64Key = bytes_to_num(datain, 6);

    // variables
    uint16_t i, j, len;
    static uint16_t dmin, dmax;
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0, nt1, nt2, nttest, ks1;
    uint8_t par[1] = {0x00};
    uint32_t target_nt[2] = {0x00}, target_ks[2] = {0x00};

    uint8_t par_array[4] = {0x00};
    uint16_t ncount = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};

    uint32_t auth1_time, auth2_time;
    static uint16_t delta_time = 0;

    LED_A_ON();
    LED_C_OFF();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // free eventually allocated BigBuf memory
    BigBuf_free();
    BigBuf_Clear_ext(false);

    if (arg2) clear_trace();
    set_tracing(true);

    // statistics on nonce distance
    int16_t isOK = 0;
#define NESTED_MAX_TRIES 12
    if (arg2) { // calibrate: for first call only. Otherwise reuse previous calibration
        LED_B_ON();
        WDT_HIT();

        uint16_t unsuccessfull_tries = 0;
        uint16_t davg = 0;
        dmax = 0;
        dmin = 2000;
        delta_time = 0;
        uint16_t rtr;
        for (rtr = 0; rtr < 17; rtr++) {

            // Test if the action was cancelled
            if (BUTTON_PRESS()) {
                isOK = -2;
                break;
            }

            // prepare next select. No need to power down the card.
            if (mifare_classic_halt(pcs, cuid)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Halt error");
                rtr--;
                continue;
            }

            if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Can't select card");
                rtr--;
                continue;
            };

            auth1_time = 0;
            if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, &auth1_time)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Auth1 error");
                rtr--;
                continue;
            };
            auth2_time = (delta_time) ? auth1_time + delta_time : 0;

            if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_NESTED, &nt2, &auth2_time)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Auth2 error");
                rtr--;
                continue;
            };

            uint32_t nttmp = prng_successor(nt1, 100); //NXP Mifare is typical around 840,but for some unlicensed/compatible mifare card this can be 160
            for (i = 101; i < 1200; i++) {
                nttmp = prng_successor(nttmp, 1);
                if (nttmp == nt2) break;
            }

            if (i != 1200) {
                if (rtr != 0) {
                    davg += i;
                    dmin = MIN(dmin, i);
                    dmax = MAX(dmax, i);
                } else {
                    delta_time = auth2_time - auth1_time + 32;  // allow some slack for proper timing
                }
                if (DBGLEVEL >= 3) Dbprintf("Nested: calibrating... ntdist=%d", i);
            } else {
                unsuccessfull_tries++;
                if (unsuccessfull_tries > NESTED_MAX_TRIES) { // card isn't vulnerable to nested attack (random numbers are not predictable)
                    isOK = -3;
                }
            }
        }

        davg = (davg + (rtr - 1) / 2) / (rtr - 1);

        if (DBGLEVEL >= 3) Dbprintf("rtr=%d isOK=%d min=%d max=%d avg=%d, delta_time=%d", rtr, isOK, dmin, dmax, davg, delta_time);

        dmin = davg - 2;
        dmax = davg + 2;

        LED_B_OFF();
    }
//  -------------------------------------------------------------------------------------------------

    LED_C_ON();

    //  get crypted nonces for target sector
    for (i = 0; i < 2 && !isOK; i++) { // look for exactly two different nonces

        target_nt[i] = 0;
        while (target_nt[i] == 0) { // continue until we have an unambiguous nonce

            // prepare next select. No need to power down the card.
            if (mifare_classic_halt(pcs, cuid)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Halt error");
                continue;
            }

            if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Can't select card");
                continue;
            };

            auth1_time = 0;
            if (mifare_classic_authex(pcs, cuid, blockNo, keyType, ui64Key, AUTH_FIRST, &nt1, &auth1_time)) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Auth1 error");
                continue;
            };

            // nested authentication
            auth2_time = auth1_time + delta_time;

            len = mifare_sendcmd_short(pcs, AUTH_NESTED, 0x60 + (targetKeyType & 0x01), targetBlockNo, receivedAnswer, par, &auth2_time);
            if (len != 4) {
                if (DBGLEVEL >= 2) Dbprintf("Nested: Auth2 error len=%d", len);
                continue;
            };

            nt2 = bytes_to_num(receivedAnswer, 4);
            if (DBGLEVEL >= 3) Dbprintf("Nonce#%d: Testing nt1=%08x nt2enc=%08x nt2par=%02x", i + 1, nt1, nt2, par[0]);

            // Parity validity check
            for (j = 0; j < 4; j++) {
                par_array[j] = (oddparity8(receivedAnswer[j]) != ((par[0] >> (7 - j)) & 0x01));
            }

            ncount = 0;
            nttest = prng_successor(nt1, dmin - 1);
            for (j = dmin; j < dmax + 1; j++) {
                nttest = prng_successor(nttest, 1);
                ks1 = nt2 ^ nttest;

                if (valid_nonce(nttest, nt2, ks1, par_array)) {
                    if (ncount > 0) { // we are only interested in disambiguous nonces, try again
                        if (DBGLEVEL >= 3) Dbprintf("Nonce#%d: dismissed (ambigous), ntdist=%d", i + 1, j);
                        target_nt[i] = 0;
                        break;
                    }
                    target_nt[i] = nttest;
                    target_ks[i] = ks1;
                    ncount++;
                    if (i == 1 && target_nt[1] == target_nt[0]) { // we need two different nonces
                        target_nt[i] = 0;
                        if (DBGLEVEL >= 3) Dbprintf("Nonce#2: dismissed (= nonce#1), ntdist=%d", j);
                        break;
                    }
                    if (DBGLEVEL >= 3) Dbprintf("Nonce#%d: valid, ntdist=%d", i + 1, j);
                }
            }
            if (target_nt[i] == 0 && j == dmax + 1 && DBGLEVEL >= 3) Dbprintf("Nonce#%d: dismissed (all invalid)", i + 1);
        }
    }

    LED_C_OFF();

    crypto1_destroy(pcs);

    uint8_t buf[4 + 4 * 4] = {0};
    memcpy(buf, &cuid, 4);
    memcpy(buf + 4, &target_nt[0], 4);
    memcpy(buf + 8, &target_ks[0], 4);
    memcpy(buf + 12, &target_nt[1], 4);
    memcpy(buf + 16, &target_ks[1], 4);

    LED_B_ON();
    reply_mix(CMD_ACK, isOK, 0, targetBlockNo + (targetKeyType * 0x100), buf, sizeof(buf));
    LED_B_OFF();

    if (DBGLEVEL >= 3) DbpString("NESTED FINISHED");

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}

//-----------------------------------------------------------------------------
// MIFARE check keys. key count up to 85.
//
//-----------------------------------------------------------------------------
typedef struct sector_t {
    uint8_t keyA[6];
    uint8_t keyB[6];
} sector_t;

typedef struct chk_t {
    uint64_t key;
    uint32_t cuid;
    uint8_t cl;
    uint8_t block;
    uint8_t keyType;
    uint8_t *uid;
    struct Crypto1State *pcs;
} chk_t;

// checks one key.
// fast select,  tries 5 times to select
//
// return:
//  2 = failed to select.
//  1 = wrong key
//  0 = correct key
uint8_t chkKey(struct chk_t *c) {
    uint8_t i = 0, res = 2;
    while (i < 5) {
        // this part is from Piwi's faster nonce collecting part in Hardnested.
        // assume: fast select
        if (!iso14443a_fast_select_card(c->uid, c->cl)) {
            ++i;
            continue;
        }
        res = mifare_classic_authex(c->pcs, c->cuid, c->block, c->keyType, c->key, AUTH_FIRST, NULL, NULL);

        CHK_TIMEOUT();

        // if successfull auth, send HALT
        // if ( !res )
        // mifare_classic_halt_ex(c->pcs);
        break;
    }
    return res;
}

uint8_t chkKey_readb(struct chk_t *c, uint8_t *keyb) {

    if (!iso14443a_fast_select_card(c->uid, c->cl))
        return 2;

    if (mifare_classic_authex(c->pcs, c->cuid, c->block, 0, c->key, AUTH_FIRST, NULL, NULL))
        return 1;

    uint8_t data[16] = {0x00};
    uint8_t res = mifare_classic_readblock(c->pcs, c->cuid, c->block, data);

    // successful read
    if (!res) {
        // data was something else than zeros.
        if (memcmp(data + 10, "\x00\x00\x00\x00\x00\x00", 6) != 0) {
            memcpy(keyb, data + 10, 6);
            res = 0;
        } else {
            res = 3;
        }
        mifare_classic_halt_ex(c->pcs);
    }
    return res;
}

void chkKey_scanA(struct chk_t *c, struct sector_t *k_sector, uint8_t *found, uint8_t *sectorcnt, uint8_t *foundkeys) {
    for (uint8_t s = 0; s < *sectorcnt; s++) {

        // skip already found A keys
        if (found[(s * 2)])
            continue;

        c->block = FirstBlockOfSector(s);
        if (chkKey(c) == 0) {
            num_to_bytes(c->key, 6, k_sector[s].keyA);
            found[(s * 2)] = 1;
            ++*foundkeys;

            if (DBGLEVEL >= 3) Dbprintf("ChkKeys_fast: Scan A found (%d)", c->block);
        }
    }
}

void chkKey_scanB(struct chk_t *c, struct sector_t *k_sector, uint8_t *found, uint8_t *sectorcnt, uint8_t *foundkeys) {
    for (uint8_t s = 0; s < *sectorcnt; s++) {

        // skip already found B keys
        if (found[(s * 2) + 1])
            continue;

        c->block = FirstBlockOfSector(s);
        if (chkKey(c) == 0) {
            num_to_bytes(c->key, 6, k_sector[s].keyB);
            found[(s * 2) + 1] = 1;
            ++*foundkeys;

            if (DBGLEVEL >= 3) Dbprintf("ChkKeys_fast: Scan B found (%d)", c->block);
        }
    }
}

// loop all A keys,
// when A is found but not B,  try to read B.
void chkKey_loopBonly(struct chk_t *c, struct sector_t *k_sector, uint8_t *found, uint8_t *sectorcnt, uint8_t *foundkeys) {

    // read Block B, if A is found.
    for (uint8_t s = 0; s < *sectorcnt; ++s) {

        if (found[(s * 2)] && found[(s * 2) + 1])
            continue;

        c->block = (FirstBlockOfSector(s) + NumBlocksPerSector(s) - 1);

        // A but not B
        if (found[(s * 2)]  &&  !found[(s * 2) + 1]) {
            c->key = bytes_to_num(k_sector[s].keyA, 6);
            uint8_t status = chkKey_readb(c, k_sector[s].keyB);
            if (status == 0) {
                found[(s * 2) + 1] = 1;
                ++*foundkeys;

                if (DBGLEVEL >= 3) Dbprintf("ChkKeys_fast: Reading B found (%d)", c->block);

                // try quick find all B?
                // assume: keys comes in groups. Find one B, test against all B.
                c->key = bytes_to_num(k_sector[s].keyB, 6);
                c->keyType = 1;
                chkKey_scanB(c, k_sector, found, sectorcnt, foundkeys);
            }
        }
    }
}



// get Chunks of keys, to test authentication against card.
// arg0 = antal sectorer
// arg0 = first time
// arg1 = clear trace
// arg2 = antal nycklar i keychunk
// datain = keys as array
void MifareChkKeys_fast(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint8_t *datain) {

    // first call or
    uint8_t sectorcnt = arg0 & 0xFF; // 16;
    uint8_t firstchunk = (arg0 >> 8) & 0xF;
    uint8_t lastchunk = (arg0 >> 12) & 0xF;
    uint8_t strategy = arg1 & 0xFF;
    uint8_t use_flashmem = (arg1 >> 8) & 0xFF;
    uint16_t keyCount = arg2 & 0xFF;
    uint8_t status = 0;

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;
    struct chk_t chk_data;

    uint8_t allkeys = sectorcnt << 1;

    static uint32_t cuid = 0;
    static uint8_t cascade_levels = 0;
    static uint8_t foundkeys = 0;
    static sector_t k_sector[80];
    static uint8_t found[80];
    static uint8_t *uid;

#ifdef WITH_FLASH
    if (use_flashmem) {
        BigBuf_free();
        uint16_t isok = 0;
        uint8_t size[2] = {0x00, 0x00};
        isok = Flash_ReadData(DEFAULT_MF_KEYS_OFFSET, size, 2);
        if (isok != 2)
            goto OUT;

        keyCount = size[1] << 8 | size[0];

        if (keyCount == 0 || keyCount == 0xFFFF)
            goto OUT;

        datain = BigBuf_malloc(keyCount * 6);
        if (datain == NULL)
            goto OUT;

        isok = Flash_ReadData(DEFAULT_MF_KEYS_OFFSET + 2, datain, keyCount * 6);
        if (isok != keyCount * 6)
            goto OUT;

    }
#endif

    if (uid == NULL || firstchunk) {
        uid = BigBuf_malloc(10);
        if (uid == NULL)
            goto OUT;
    }

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    LEDsoff();
    LED_A_ON();

    if (firstchunk) {
        clear_trace();
        set_tracing(false);

        memset(k_sector, 0x00, 480 + 10);
        memset(found, 0x00, sizeof(found));
        foundkeys = 0;

        iso14a_card_select_t card_info;
        if (!iso14443a_select_card(uid, &card_info, &cuid, true, 0, true)) {
            if (DBGLEVEL >= 1) Dbprintf("ChkKeys_fast: Can't select card (ALL)");
            goto OUT;
        }

        switch (card_info.uidlen) {
            case 4 :
                cascade_levels = 1;
                break;
            case 7 :
                cascade_levels = 2;
                break;
            case 10:
                cascade_levels = 3;
                break;
            default:
                break;
        }

        CHK_TIMEOUT();
    }

    // set check struct.
    chk_data.uid = uid;
    chk_data.cuid = cuid;
    chk_data.cl = cascade_levels;
    chk_data.pcs = pcs;
    chk_data.block = 0;

    // keychunk loop - depth first one sector.
    if (strategy == 1 || use_flashmem) {

        uint8_t newfound = foundkeys;

        uint16_t lastpos = 0;
        uint16_t s_point = 0;
        // Sector main loop
        // keep track of how many sectors on card.
        for (uint8_t s = 0; s < sectorcnt; ++s) {

            if (found[(s * 2)] && found[(s * 2) + 1])
                continue;

            for (uint16_t i = s_point; i < keyCount; ++i) {

                // Allow button press / usb cmd to interrupt device
                if (BUTTON_PRESS() && !data_available()) {
                    goto OUT;
                }

                // found all keys?
                if (foundkeys == allkeys)
                    goto OUT;

                WDT_HIT();

                // assume: block0,1,2 has more read rights in accessbits than the sectortrailer. authenticating against block0 in each sector
                chk_data.block = FirstBlockOfSector(s);

                // new key
                chk_data.key = bytes_to_num(datain + i * 6, 6);

                // skip already found A keys
                if (!found[(s * 2)]) {
                    chk_data.keyType = 0;
                    status = chkKey(&chk_data);
                    if (status == 0) {
                        memcpy(k_sector[s].keyA, datain + i * 6, 6);
                        found[(s * 2)] = 1;
                        ++foundkeys;

                        chkKey_scanA(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        // read Block B, if A is found.
                        chkKey_loopBonly(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        chk_data.keyType = 1;
                        chkKey_scanB(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        chk_data.keyType = 0;
                        chk_data.block = FirstBlockOfSector(s);

                        if (use_flashmem) {
                            if (lastpos != i && lastpos != 0) {
                                if (i - lastpos < 0xF) {
                                    s_point = i & 0xFFF0;
                                }
                            } else {
                                lastpos = i;
                            }
                        }
                    }
                }

                // skip already found B keys
                if (!found[(s * 2) + 1]) {
                    chk_data.keyType = 1;
                    status = chkKey(&chk_data);
                    if (status == 0) {
                        memcpy(k_sector[s].keyB, datain + i * 6, 6);
                        found[(s * 2) + 1] = 1;
                        ++foundkeys;

                        chkKey_scanB(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        if (use_flashmem) {
                            if (lastpos != i && lastpos != 0) {

                                if (i - lastpos < 0xF)
                                    s_point = i & 0xFFF0;
                            } else {
                                lastpos = i;
                            }
                        }
                    }
                }

                if (found[(s * 2)] && found[(s * 2) + 1])
                    break;

            } // end keys test loop - depth first

            // assume1. if no keys found in first sector, get next keychunk from client
            if (!use_flashmem && (newfound - foundkeys == 0))
                goto OUT;

        } // end loop - sector
    } // end strategy 1

    if (foundkeys == allkeys)
        goto OUT;

    if (strategy == 2 || use_flashmem) {

        // Keychunk loop
        for (uint16_t i = 0; i < keyCount; i++) {

            // Allow button press / usb cmd to interrupt device
            if (BUTTON_PRESS() && !data_available()) break;

            // found all keys?
            if (foundkeys == allkeys)
                goto OUT;

            WDT_HIT();

            // new key
            chk_data.key = bytes_to_num(datain + i * 6, 6);

            // Sector main loop
            // keep track of how many sectors on card.
            for (uint8_t s = 0; s < sectorcnt; ++s) {

                if (found[(s * 2)] && found[(s * 2) + 1]) continue;

                // found all keys?
                if (foundkeys == allkeys)
                    goto OUT;

                // assume: block0,1,2 has more read rights in accessbits than the sectortrailer. authenticating against block0 in each sector
                chk_data.block = FirstBlockOfSector(s);

                // skip already found A keys
                if (!found[(s * 2)]) {
                    chk_data.keyType = 0;
                    status = chkKey(&chk_data);
                    if (status == 0) {
                        memcpy(k_sector[s].keyA, datain + i * 6, 6);
                        found[(s * 2)] = 1;
                        ++foundkeys;

                        chkKey_scanA(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        // read Block B, if A is found.
                        chkKey_loopBonly(&chk_data, k_sector, found, &sectorcnt, &foundkeys);

                        chk_data.block = FirstBlockOfSector(s);
                    }
                }

                // skip already found B keys
                if (!found[(s * 2) + 1]) {
                    chk_data.keyType = 1;
                    status = chkKey(&chk_data);
                    if (status == 0) {
                        memcpy(k_sector[s].keyB, datain + i * 6, 6);
                        found[(s * 2) + 1] = 1;
                        ++foundkeys;

                        chkKey_scanB(&chk_data, k_sector, found, &sectorcnt, &foundkeys);
                    }
                }
            } // end loop sectors
        } // end loop keys
    } // end loop strategy 2
OUT:
    LEDsoff();

    crypto1_destroy(pcs);

    // All keys found, send to client, or last keychunk from client
    if (foundkeys == allkeys || lastchunk) {

        uint64_t foo = 0;
        for (uint8_t m = 0; m < 64; m++) {
            foo |= ((uint64_t)(found[m] & 1) << m);
        }

        uint16_t bar = 0;
        uint8_t j = 0;
        for (uint8_t m = 64; m < ARRAYLEN(found); m++) {
            bar |= ((uint16_t)(found[m] & 1) << j++);
        }

        uint8_t *tmp =  BigBuf_malloc(480 + 10);
        memcpy(tmp, k_sector, sectorcnt * sizeof(sector_t));
        num_to_bytes(foo, 8, tmp + 480);
        tmp[488] = bar & 0xFF;
        tmp[489] = bar >> 8 & 0xFF;

        reply_old(CMD_ACK, foundkeys, 0, 0, tmp, 480 + 10);

        set_tracing(false);
        FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
        BigBuf_free();
        BigBuf_Clear_ext(false);
		
		// special trick ecfill
		if (use_flashmem && foundkeys == allkeys) {
			
			uint8_t block[16] = {0};
		    for (int i = 0; i < sectorcnt; i++) {
				
				uint8_t blockno;
				if (i < 32) {
					blockno = (i * 4) ^ 0x3;
    } else {
					blockno = (32 * 4 + (i - 32) * 16) ^ 0xF;
				}
				// get ST
				emlGetMem(block, blockno, 1);

				memcpy(block, k_sector[i].keyA, 6);
				memcpy(block + 10, k_sector[i].keyB, 6);
				
				emlSetMem_xt(block, blockno, 1, sizeof(block));
			}
		    int oldbg = DBGLEVEL;
			DBGLEVEL = DBG_NONE;
			MifareECardLoad(sectorcnt, 0);
			MifareECardLoad(sectorcnt, 1);
			DBGLEVEL = oldbg;
		}
    } else {
        // partial/none keys found
        reply_mix(CMD_ACK, foundkeys, 0, 0, 0, 0);
    }
}

void MifareChkKeys(uint8_t *datain) {

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    uint8_t uid[10] = {0x00};

    uint64_t key = 0;
    uint32_t cuid = 0;
    int i, res;
    uint8_t cascade_levels = 0;
    struct {
        uint8_t key[6];
        bool found;
    } PACKED keyresult;
    keyresult.found = false;
    uint8_t blockNo, keyType, keyCount;
    bool clearTrace, have_uid = false;

    keyType = datain[0];
    blockNo = datain[1];
    clearTrace = datain[2];
    keyCount = datain[3];
    datain += 4;

    LEDsoff();
    LED_A_ON();

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    if (clearTrace)
        clear_trace();

    set_tracing(true);

    for (i = 0; i < keyCount; i++) {

        // Iceman: use piwi's faster nonce collecting part in hardnested.
        if (!have_uid) { // need a full select cycle to get the uid first
            iso14a_card_select_t card_info;
            if (!iso14443a_select_card(uid, &card_info, &cuid, true, 0, true)) {
                if (DBGLEVEL >= 1) Dbprintf("ChkKeys: Can't select card (ALL)");
                --i; // try same key once again
                continue;
            }
            switch (card_info.uidlen) {
                case 4 :
                    cascade_levels = 1;
                    break;
                case 7 :
                    cascade_levels = 2;
                    break;
                case 10:
                    cascade_levels = 3;
                    break;
                default:
                    break;
            }
            have_uid = true;
        } else { // no need for anticollision. We can directly select the card
            if (!iso14443a_select_card(uid, NULL, NULL, false, cascade_levels, true)) {
                if (DBGLEVEL >= 1) Dbprintf("ChkKeys: Can't select card (UID)");
                --i; // try same key once again
                continue;
            }
        }

        key = bytes_to_num(datain + i * 6, 6);
        res = mifare_classic_auth(pcs, cuid, blockNo, keyType, key, AUTH_FIRST);

        CHK_TIMEOUT();

        if (res)
            continue;
        memcpy(keyresult.key, datain + i * 6, 6);
        keyresult.found = true;
        break;
    }

    LED_B_ON();

    reply_ng(CMD_HF_MIFARE_CHKKEYS, PM3_SUCCESS, (uint8_t *)&keyresult, sizeof(keyresult));
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();

    set_tracing(false);
    crypto1_destroy(pcs);
}

//-----------------------------------------------------------------------------
// Work with emulator memory
//
// Note: we call FpgaDownloadAndGo(FPGA_BITSTREAM_HF) here although FPGA is not
// involved in dealing with emulator memory. But if it is called later, it might
// destroy the Emulator Memory.
//-----------------------------------------------------------------------------

void MifareEMemClr(void) {
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    emlClearMem();
}

void MifareEMemSet(uint8_t blockno, uint8_t blockcnt, uint8_t blockwidth, uint8_t *datain) {
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    if (blockwidth == 0)
        blockwidth = 16; // backwards compat... default bytewidth

    emlSetMem_xt(datain, blockno, blockcnt, blockwidth); // data, block num, blocks count, block byte width
}

void MifareEMemGet(uint8_t blockno, uint8_t blockcnt) {
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    //
    size_t size = blockcnt * 16;
    if (size > PM3_CMD_DATA_SIZE) {
        reply_ng(CMD_HF_MIFARE_EML_MEMGET, PM3_EMALLOC, NULL, 0);
        return;
    }

    uint8_t *buf = BigBuf_malloc(size);

    emlGetMem(buf, blockno, blockcnt); // data, block num, blocks count (max 4)

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_EML_MEMGET, PM3_SUCCESS, buf, size);
    LED_B_OFF();
    BigBuf_free_keep_EM();
}

//-----------------------------------------------------------------------------
// Load a card into the emulator memory
//
//-----------------------------------------------------------------------------
int MifareECardLoadExt(uint8_t numSectors, uint8_t keyType) {
	int retval = MifareECardLoad(numSectors, keyType);
	reply_ng(CMD_HF_MIFARE_EML_LOAD, retval, NULL, 0);
	return retval;
}

int MifareECardLoad(uint8_t numSectors, uint8_t keyType) {

    uint32_t cuid = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs;
    pcs = &mpcs;

    // variables
    uint8_t dataoutbuf[16] = {0x00};
    uint8_t dataoutbuf2[16] = {0x00};
    uint8_t uid[10] = {0x00};

    LED_A_ON();
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    int retval;

    if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
        retval = PM3_ESOFT;
        if (DBGLEVEL > DBG_ERROR) Dbprintf("Can't select card");
		goto out;
    }

    for (uint8_t sectorNo = 0; sectorNo < numSectors; sectorNo++) {
        uint64_t ui64Key = emlGetKey(sectorNo, keyType);
        if (sectorNo == 0) {
            if (mifare_classic_auth(pcs, cuid, FirstBlockOfSector(sectorNo), keyType, ui64Key, AUTH_FIRST)) {
                if (DBGLEVEL > DBG_ERROR) Dbprintf("Sector[%2d]. Auth error", sectorNo);
                break;
            }
        } else {
            if (mifare_classic_auth(pcs, cuid, FirstBlockOfSector(sectorNo), keyType, ui64Key, AUTH_NESTED)) {
                retval = PM3_ESOFT;
                if (DBGLEVEL > DBG_ERROR) Dbprintf("Sector[%2d]. Auth nested error", sectorNo);
                goto out;
            }
        }

        for (uint8_t blockNo = 0; blockNo < NumBlocksPerSector(sectorNo); blockNo++) {
            if (mifare_classic_readblock(pcs, cuid, FirstBlockOfSector(sectorNo) + blockNo, dataoutbuf)) {
                retval = PM3_ESOFT;
                if (DBGLEVEL > DBG_ERROR) Dbprintf("Error reading sector %2d block %2d", sectorNo, blockNo);
                break;
            }
                if (blockNo < NumBlocksPerSector(sectorNo) - 1) {
                    emlSetMem(dataoutbuf, FirstBlockOfSector(sectorNo) + blockNo, 1);
                } else { // sector trailer, keep the keys, set only the AC
                    emlGetMem(dataoutbuf2, FirstBlockOfSector(sectorNo) + blockNo, 1);
                    memcpy(&dataoutbuf2[6], &dataoutbuf[6], 4);
                    emlSetMem(dataoutbuf2,  FirstBlockOfSector(sectorNo) + blockNo, 1);
                }
            }
        }

    if (mifare_classic_halt(pcs, cuid)) {
        if (DBGLEVEL > DBG_ERROR)
            Dbprintf("Halt error");
	}

	if (DBGLEVEL >= DBG_INFO) DbpString("Emulator fill sectors finished");

out:
    crypto1_destroy(pcs);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
    return retval;
}


//-----------------------------------------------------------------------------
// Work with "magic Chinese" card (email him: ouyangweidaxian@live.cn)
//
// PARAMS - workFlags
// bit 0 - need get UID
// bit 1 - need wupC
// bit 2 - need HALT after sequence
// bit 3 - need turn on FPGA before sequence
// bit 4 - need turn off FPGA
// bit 5 - need to set datain instead of issuing USB reply (called via ARM for StandAloneMode14a)
// bit 6 - wipe tag.
//-----------------------------------------------------------------------------
// magic uid card generation 1 commands
uint8_t wupC1[] = { MIFARE_MAGICWUPC1 };
uint8_t wupC2[] = { MIFARE_MAGICWUPC2 };
uint8_t wipeC[] = { MIFARE_MAGICWIPEC };

void MifareCSetBlock(uint32_t arg0, uint32_t arg1, uint8_t *datain) {

    // params
    uint8_t workFlags = arg0;
    uint8_t blockNo = arg1;

    // detect 1a/1b
    bool is1b = false;

    // variables
    bool isOK = false; //assume we will get an error
    uint8_t errormsg = 0x00;
    uint8_t uid[10] = {0x00};
    uint8_t data[18] = {0x00};
    uint32_t cuid = 0;

    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};

    if (workFlags & MAGIC_INIT) {
        LED_A_ON();
        LED_B_OFF();
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
        clear_trace();
        set_tracing(true);
    }

    //loop doesn't loop just breaks out if error
    while (true) {
        // read UID and return to client with write
        if (workFlags & MAGIC_UID) {
            if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
                if (DBGLEVEL >= DBG_ERROR) Dbprintf("Can't select card");
                errormsg = MAGIC_UID;
            }
            mifare_classic_halt_ex(NULL);
            break;
        }

        // wipe tag, fill it with zeros
        if (workFlags & MAGIC_WIPE) {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if (!ReaderReceive(receivedAnswer, receivedAnswerPar) || (receivedAnswer[0] != 0x0a)) {
                if (DBGLEVEL >= DBG_ERROR) Dbprintf("wupC1 error");
                errormsg = MAGIC_WIPE;
                break;
            }

            ReaderTransmit(wipeC, sizeof(wipeC), NULL);
            if (!ReaderReceive(receivedAnswer, receivedAnswerPar) || (receivedAnswer[0] != 0x0a)) {
                if (DBGLEVEL >= DBG_ERROR) Dbprintf("wipeC error");
                errormsg = MAGIC_WIPE;
                break;
            }

            mifare_classic_halt_ex(NULL);
        }

        // write block
        if (workFlags & MAGIC_WUPC) {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if (!ReaderReceive(receivedAnswer, receivedAnswerPar) || (receivedAnswer[0] != 0x0a)) {
                if (DBGLEVEL >= DBG_ERROR) Dbprintf("wupC1 error");
                errormsg = MAGIC_WUPC;
                break;
            }

            if (!is1b) {
                ReaderTransmit(wupC2, sizeof(wupC2), NULL);
                if (!ReaderReceive(receivedAnswer, receivedAnswerPar) || (receivedAnswer[0] != 0x0a)) {
                    if (DBGLEVEL >= DBG_INFO) Dbprintf("Assuming Magic Gen 1B tag. [wupC2 failed]");
                    is1b = true;
                    continue;
                }
            }
        }

        if ((mifare_sendcmd_short(NULL, 0, ISO14443A_CMD_WRITEBLOCK, blockNo, receivedAnswer, receivedAnswerPar, NULL) != 1) || (receivedAnswer[0] != 0x0a)) {
            if (DBGLEVEL >= DBG_ERROR) Dbprintf("write block send command error");
            errormsg = 4;
            break;
        }

        memcpy(data, datain, 16);
        AddCrc14A(data, 16);

        ReaderTransmit(data, sizeof(data), NULL);
        if ((ReaderReceive(receivedAnswer, receivedAnswerPar) != 1) || (receivedAnswer[0] != 0x0a)) {
            if (DBGLEVEL >= DBG_ERROR) Dbprintf("write block send data error");
            errormsg = 0;
            break;
        }

        if (workFlags & MAGIC_HALT)
            mifare_classic_halt_ex(NULL);

        isOK = true;
        break;

    } // end while

    if (isOK)
        reply_mix(CMD_ACK, 1, 0, 0, uid, sizeof(uid));
    else
        OnErrorMagic(errormsg);

    if (workFlags & MAGIC_OFF)
        OnSuccessMagic();
}

void MifareCGetBlock(uint32_t arg0, uint32_t arg1, uint8_t *datain) {

    uint8_t workFlags = arg0;
    uint8_t blockNo = arg1;
    uint8_t errormsg = 0x00;
    bool isOK = false; //assume we will get an error

    // detect 1a/1b
    bool is1b = false;

    // variables
    uint8_t data[MAX_MIFARE_FRAME_SIZE];
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0x00};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0x00};

    memset(data, 0x00, sizeof(data));

    if (workFlags & MAGIC_INIT) {
        LED_A_ON();
        LED_B_OFF();
        iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
        clear_trace();
        set_tracing(true);
    }

    //loop doesn't loop just breaks out if error or done
    while (true) {
        if (workFlags & MAGIC_WUPC) {
            ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
            if (!ReaderReceive(receivedAnswer, receivedAnswerPar) || (receivedAnswer[0] != 0x0a)) {
                if (DBGLEVEL >= DBG_ERROR) Dbprintf("wupC1 error");
                errormsg = MAGIC_WUPC;
                break;
            }

            if (!is1b)  {
                ReaderTransmit(wupC2, sizeof(wupC2), NULL);
                if (!ReaderReceive(receivedAnswer, receivedAnswerPar) || (receivedAnswer[0] != 0x0a)) {
                    if (DBGLEVEL >= DBG_INFO) Dbprintf("Assuming Magic Gen 1B tag. [wupC2 failed]");
                    is1b = true;
                    continue;
                }
            }
        }

        // read block
        if ((mifare_sendcmd_short(NULL, 0, ISO14443A_CMD_READBLOCK, blockNo, receivedAnswer, receivedAnswerPar, NULL) != 18)) {
            if (DBGLEVEL >= DBG_ERROR) Dbprintf("read block send command error");
            errormsg = 0;
            break;
        }

        memcpy(data, receivedAnswer, sizeof(data));

        // send HALT
        if (workFlags & MAGIC_HALT)
            mifare_classic_halt_ex(NULL);

        isOK = true;
        break;
    }
    // if MAGIC_DATAIN, the data stays on device side.
    if (workFlags & MAGIC_DATAIN) {
        if (isOK)
            memcpy(datain, data, sizeof(data));
    } else {
        if (isOK)
            reply_old(CMD_ACK, 1, 0, 0, data, sizeof(data));
        else
            OnErrorMagic(errormsg);
    }

    if (workFlags & MAGIC_OFF)
        OnSuccessMagic();
}

void MifareCIdent() {
#define GEN_1A 1
#define GEN_1B 2
#define GEN_2  4
#define GEN_UNFUSED 5

    // variables
    uint8_t isGen = 0;
    uint8_t rec[1] = {0x00};
    uint8_t recpar[1] = {0x00};
    uint8_t rats[4] = { ISO14443A_CMD_RATS, 0x80, 0x31, 0x73 };
    uint8_t *par = BigBuf_malloc(MAX_PARITY_SIZE);
    uint8_t *buf = BigBuf_malloc(PM3_CMD_DATA_SIZE);
    uint8_t *uid = BigBuf_malloc(10);
    uint32_t cuid = 0;
    uint8_t data[1] = {0x00};

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // Generation 1 test
    ReaderTransmitBitsPar(wupC1, 7, NULL, NULL);
    if (!ReaderReceive(rec, recpar) || (rec[0] != 0x0a)) {
        goto TEST2;
    };

    ReaderTransmit(wupC2, sizeof(wupC2), NULL);
    if (!ReaderReceive(rec, recpar) || (rec[0] != 0x0a)) {
        isGen = GEN_1B;
        goto OUT;
    };
    isGen = GEN_1A;
    goto OUT;

TEST2:
    // reset card
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    SpinDelay(100);
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    int res = iso14443a_select_card(uid, NULL, &cuid, true, 0, true);
    if (res == 2) {
        if (cuid == 0xAA55C396) {
            isGen = GEN_UNFUSED;
            goto OUT;
        }

        ReaderTransmit(rats, sizeof(rats), NULL);
        res = ReaderReceive(buf, par);
        if (memcmp(buf, "\x09\x78\x00\x91\x02\xDA\xBC\x19\x10\xF0\x05", 11) == 0) {
            isGen = GEN_2;
            goto OUT;
        }
        if (memcmp(buf, "\x0D\x78\x00\x71\x02\x88\x49\xA1\x30\x20\x15\x06\x08\x56\x3D", 15) == 0) {
            isGen = GEN_2;
        }
    };

OUT:

    data[0] = isGen;
    reply_ng(CMD_HF_MIFARE_CIDENT, PM3_SUCCESS, data, sizeof(data));
    // turns off
    OnSuccessMagic();
    BigBuf_free();
}

void OnSuccessMagic() {
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}
void OnErrorMagic(uint8_t reason) {
    //          ACK, ISOK, reason,0,0,0
    reply_mix(CMD_ACK, 0, reason, 0, 0, 0);
    OnSuccessMagic();
}

void MifareSetMod(uint8_t *datain) {

    uint8_t mod = datain[0];
    uint64_t ui64Key = bytes_to_num(datain + 1, 6);

    // variables
    uint16_t isOK = PM3_EUNDEF;
    uint8_t uid[10] = {0};
    uint32_t cuid = 0;
    struct Crypto1State mpcs = {0, 0};
    struct Crypto1State *pcs = &mpcs;
    uint8_t receivedAnswer[MAX_MIFARE_FRAME_SIZE] = {0};
    uint8_t receivedAnswerPar[MAX_MIFARE_PARITY_SIZE] = {0};

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    clear_trace();
    set_tracing(true);

    LED_A_ON();
    LED_B_OFF();
    LED_C_OFF();

    while (true) {
        if (!iso14443a_select_card(uid, NULL, &cuid, true, 0, true)) {
            if (DBGLEVEL >= 1) Dbprintf("Can't select card");
            break;
        }

        if (mifare_classic_auth(pcs, cuid, 0, 0, ui64Key, AUTH_FIRST)) {
            if (DBGLEVEL >= 1) Dbprintf("Auth error");
            break;
        }

        int respLen;
        if (((respLen = mifare_sendcmd_short(pcs, 1, 0x43, mod, receivedAnswer, receivedAnswerPar, NULL)) != 1) || (receivedAnswer[0] != 0x0a)) {
            if (DBGLEVEL >= 1) Dbprintf("SetMod error; response[0]: %hhX, len: %d", receivedAnswer[0], respLen);
            break;
        }

        if (mifare_classic_halt(pcs, cuid)) {
            if (DBGLEVEL >= 1) Dbprintf("Halt error");
            break;
        }

        isOK = PM3_SUCCESS;
        break;
    }

    crypto1_destroy(pcs);

    LED_B_ON();
    reply_ng(CMD_HF_MIFARE_SETMOD, isOK, NULL, 0);

    LED_B_OFF();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}

//
// DESFIRE
//
void Mifare_DES_Auth1(uint8_t arg0, uint8_t *datain) {
    uint8_t dataout[12] = {0x00};
    uint8_t uid[10] = {0x00};
    uint32_t cuid = 0;

    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);
    clear_trace();
    set_tracing(true);

    int len = iso14443a_select_card(uid, NULL, &cuid, true, 0, false);
    if (!len) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Can't select card");
        OnError(1);
        return;
    };

    if (mifare_desfire_des_auth1(cuid, dataout)) {
        if (DBGLEVEL >= DBG_ERROR) Dbprintf("Authentication part1: Fail.");
        OnError(4);
        return;
    }

    if (DBGLEVEL >= DBG_EXTENDED) DbpString("AUTH 1 FINISHED");
    reply_mix(CMD_ACK, 1, cuid, 0, dataout, sizeof(dataout));
}

void Mifare_DES_Auth2(uint32_t arg0, uint8_t *datain) {
    uint32_t cuid = arg0;
    uint8_t key[16] = {0x00};
    uint8_t dataout[12] = {0x00};
    uint8_t isOK = 0;

    memcpy(key, datain, 16);

    isOK = mifare_desfire_des_auth2(cuid, key, dataout);

    if (isOK) {
        if (DBGLEVEL >= DBG_EXTENDED) Dbprintf("Authentication part2: Failed");
        OnError(4);
        return;
    }

    if (DBGLEVEL >= DBG_EXTENDED) DbpString("AUTH 2 FINISHED");

    reply_old(CMD_ACK, isOK, 0, 0, dataout, sizeof(dataout));
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
    set_tracing(false);
}
