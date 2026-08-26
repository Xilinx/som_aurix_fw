/**
 * @file    FwUpdate.c
 * @brief   UART-based firmware update protocol state machine
 *
 * Implements the framed protocol defined in FwUpdate.h.
 * Called from the main loop via FwUpdate_Run().
 */

#include "FwUpdate.h"
#include "Uart_Xfer.h"
#include "PFlash.h"
#include "DFlash.h"
#include "BootValid.h"
#include "Swap.h"
#include "NvLog.h"
#include "Stm_Timer.h"
#include "Uart_Debug.h"
#include <string.h>
#include "IfxScuWdt.h"
#include "IfxScu_reg.h"
#include "Crc32.h"

/* ================================================================== */
/*  Internal state                                                    */
/* ================================================================== */

static FwUpdate_State_t  s_state      = FWUPDATE_IDLE;
static FwUpdate_Error_t  s_lastError  = FWUPDATE_ERR_NONE;

/* Session parameters (from HEADER) */
static uint32 s_imageSize    = 0u;
static uint32 s_imageCrc     = 0u;
static uint32 s_targetBank   = 0u;
static uint32 s_totalChunks  = 0u;
static uint32 s_nextSeqNum   = 0u;
static uint32 s_writeAddr    = 0u;
static uint32 s_lastChunkMs  = 0u;

/* CRC accumulator for full-image verify */
static uint32 s_runningCrc   = 0xFFFFFFFFu;

/* Receive buffer — large enough for one DATA frame */
static uint8  s_frameBuf[FWUPDATE_CHUNK_SIZE + 8u]
              __attribute__((aligned(4)));

/* ================================================================== */
/*  CRC-32 (standard Ethernet polynomial)                             */
/* ================================================================== */

static const uint32 s_crcTable[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
    0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u, 0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
    0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
    0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u, 0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
    0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
    0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u, 0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
    0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
    0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu, 0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
    0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
    0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u, 0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
    0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
    0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au, 0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
    0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
    0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu, 0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
    0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
    0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u, 0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
    0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
    0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u, 0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
    0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
    0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u, 0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
    0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
    0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u, 0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

/* ================================================================== */
/*  Protocol helpers                                                  */
/* ================================================================== */

static void prv_SendAck(void)
{
    uint32 magic = FWUPDATE_ACK_MAGIC;
    UartXfer_Write((const uint8 *)&magic, 4u);
}

static void prv_SendNak(FwUpdate_Error_t err)
{
    uint32 magic = FWUPDATE_NAK_MAGIC;
    uint8  code  = (uint8)err;
    UartXfer_Write((const uint8 *)&magic, 4u);
    UartXfer_Write(&code, 1u);
    s_lastError = err;
}

static uint32 prv_ReadU32(const uint8 *p)
{
    return (uint32)p[0]
         | ((uint32)p[1] << 8u)
         | ((uint32)p[2] << 16u)
         | ((uint32)p[3] << 24u);
}

/* ================================================================== */
/*  State machine                                                     */
/* ================================================================== */

static void prv_HandleIdle(void)
{
    if (UartXfer_Available() < 4u)
        return;

    uint8 syncBuf[4];
    UartXfer_Read(syncBuf, 4u);
    uint32 magic = prv_ReadU32(syncBuf);

    if (magic == FWUPDATE_SYNC_MAGIC)
    {
        Debug_Print("[FWUP] SYNC received\r\n");
        prv_SendAck();
        s_state = FWUPDATE_HEADER;
        s_lastChunkMs = Stm_GetTimeMs();

        NvLog_WriteU32(NVLOG_EVT_FWUPDATE_START, NVLOG_SRC_FWUPDATE,
                       NVLOG_SEV_INFO, 0u);
    }
    /* else: not SYNC — discard and keep scanning */
}

static void prv_HandleHeader(void)
{
    if (UartXfer_Available() < 12u)
    {
        if ((Stm_GetTimeMs() - s_lastChunkMs) > FWUPDATE_CHUNK_TIMEOUT_MS)
        {
            prv_SendNak(FWUPDATE_ERR_TIMEOUT);
            s_state = FWUPDATE_ERROR;
        }
        return;
    }

    uint8 hdrBuf[12];
    UartXfer_Read(hdrBuf, 12u);

    s_imageSize  = prv_ReadU32(&hdrBuf[0]);
    s_imageCrc   = prv_ReadU32(&hdrBuf[4]);
    s_targetBank = prv_ReadU32(&hdrBuf[8]);

    Debug_Printf("[FWUP] HEADER: size=%u crc=0x%08X bank=%u\r\n",
                 (unsigned)s_imageSize, (unsigned)s_imageCrc,
                 (unsigned)s_targetBank);

    /* Validate image size */
    uint32 maxSize = (s_targetBank == 0u) ? PFLASH_BANK_A_SIZE : PFLASH_BANK_B_SIZE;
    if (s_imageSize == 0u || s_imageSize > maxSize)
    {
        prv_SendNak(FWUPDATE_ERR_IMG_TOO_BIG);
        s_state = FWUPDATE_ERROR;
        return;
    }

    /* Calculate chunks */
    s_totalChunks = (s_imageSize + FWUPDATE_CHUNK_SIZE - 1u) / FWUPDATE_CHUNK_SIZE;

    /* Erase the target bank */
    s_state = FWUPDATE_ERASING;
    Debug_Print("[FWUP] Erasing target bank...\r\n");

    uint32 bankBase = (s_targetBank == 0u) ? PFLASH_BANK_A_BASE : PFLASH_BANK_B_BASE;
    PFlash_Status_t ps = PFlash_EraseBank(bankBase);
    if (ps != PFLASH_OK)
    {
        Debug_Printf("[FWUP] Erase failed: %u\r\n", (unsigned)ps);
        prv_SendNak(FWUPDATE_ERR_ERASE_FAIL);
        s_state = FWUPDATE_ERROR;
        return;
    }

    Debug_Print("[FWUP] Erase complete\r\n");
    prv_SendAck();

    s_nextSeqNum  = 0u;
    s_writeAddr   = bankBase;
    s_runningCrc  = Crc32_Init();
    s_lastChunkMs = Stm_GetTimeMs();
    s_state       = FWUPDATE_RECEIVING;
}

static void prv_HandleReceiving(void)
{
    /* DATA frame: seqNum(4) + chunkCrc(4) + data(256) = 264 bytes */
    uint32 frameSize = 4u + 4u + FWUPDATE_CHUNK_SIZE;

    if (UartXfer_Available() < frameSize)
    {
        if ((Stm_GetTimeMs() - s_lastChunkMs) > FWUPDATE_CHUNK_TIMEOUT_MS)
        {
            prv_SendNak(FWUPDATE_ERR_TIMEOUT);
            s_state = FWUPDATE_ERROR;
        }
        return;
    }

    UartXfer_Read(s_frameBuf, frameSize);

    uint32 seqNum   = prv_ReadU32(&s_frameBuf[0]);
    uint32 chunkCrc = prv_ReadU32(&s_frameBuf[4]);
    uint8 *pData    = &s_frameBuf[8];

    /* Verify sequence number */
    if (seqNum != s_nextSeqNum)
    {
        Debug_Printf("[FWUP] Seq mismatch: got %u, expected %u\r\n",
                     (unsigned)seqNum, (unsigned)s_nextSeqNum);
        prv_SendNak(FWUPDATE_ERR_SEQ_NUM);
        /* Don't go to ERROR — let host retry */
        return;
    }

    /* Verify chunk CRC */
    uint32 computed = Crc32_Final(Crc32_Update(Crc32_Init(), pData, FWUPDATE_CHUNK_SIZE));
    if (computed != chunkCrc)
    {
        Debug_Printf("[FWUP] Chunk CRC mismatch: 0x%08X != 0x%08X\r\n",
                     (unsigned)computed, (unsigned)chunkCrc);
        prv_SendNak(FWUPDATE_ERR_CHUNK_CRC);
        return;
    }

    /* Program this chunk to PFlash */
    PFlash_Status_t ps = PFlash_WritePage256(s_writeAddr, pData);
    if (ps != PFLASH_OK)
    {
        Debug_Printf("[FWUP] Write failed at 0x%08X: %u\r\n",
                     (unsigned)s_writeAddr, (unsigned)ps);
        prv_SendNak(FWUPDATE_ERR_WRITE_FAIL);
        s_state = FWUPDATE_ERROR;
        return;
    }

    /* Update running CRC */
    s_runningCrc = Crc32_Update(s_runningCrc, pData, FWUPDATE_CHUNK_SIZE);

    /* Advance */
    s_writeAddr  += FWUPDATE_CHUNK_SIZE;
    s_nextSeqNum++;
    s_lastChunkMs = Stm_GetTimeMs();

    prv_SendAck();

    /* Last chunk? */
    if (s_nextSeqNum >= s_totalChunks)
    {
        s_state = FWUPDATE_VERIFYING;
    }
}

static void prv_HandleVerifying(void)
{
    uint32 finalCrc = Crc32_Final(s_runningCrc);

    Debug_Printf("[FWUP] Verify: computed=0x%08X expected=0x%08X\r\n",
                 (unsigned)finalCrc, (unsigned)s_imageCrc);

    if (finalCrc != s_imageCrc)
    {
        prv_SendNak(FWUPDATE_ERR_IMG_CRC);
        s_state = FWUPDATE_ERROR;
        return;
    }

    s_state = FWUPDATE_COMMITTING;

    /* Write SOTA metadata to DFlash */
    DFlash_SotaMeta_t meta;
    meta.magic         = DFLASH_SOTA_MAGIC;
    meta.pendingUpdate = 1u;
    meta.bootCounter   = 0u;
    meta.reserved0     = 0u;
    meta.imageCrc      = s_imageCrc;
    meta.activeBank    = (s_targetBank == 0u) ? 0xAAu : 0x55u;
    meta.reserved1     = 0u;
    meta.reserved2     = 0u;

    DFlash_Status_t ds = DFlash_WriteSotaMeta(&meta);
    if (ds != DFLASH_OK)
    {
        prv_SendNak(FWUPDATE_ERR_META_FAIL);
        s_state = FWUPDATE_ERROR;
        return;
    }

    Debug_Print("[FWUP] SOTA metadata written\r\n");

    NvLog_WriteU32(NVLOG_EVT_FWUPDATE_OK, NVLOG_SRC_FWUPDATE,
                   NVLOG_SEV_INFO, s_imageSize);
    NvLog_SealSlot(NVLOG_EVT_FWUPDATE_OK);

    prv_SendAck();

    s_state = FWUPDATE_DONE;
    Debug_Print("[FWUP] Update complete — resetting...\r\n");

    /* Trigger system reset to boot from new image */
    Stm_DelayMs(100u);  /* Let UART flush */
    {
        uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();
        IfxScuWdt_clearSafetyEndinit(pw);
        MODULE_SCU.RSTCON.B.SW = 1u;
        IfxScuWdt_setSafetyEndinit(pw);
        /* Should not reach here */
        while(1) {}
    }
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void FwUpdate_Init(void)
{
    s_state     = FWUPDATE_IDLE;
    s_lastError = FWUPDATE_ERR_NONE;
}

FwUpdate_State_t FwUpdate_Run(void)
{
    switch (s_state)
    {
        case FWUPDATE_IDLE:
            prv_HandleIdle();
            break;

        case FWUPDATE_HEADER:
            prv_HandleHeader();
            break;

        case FWUPDATE_ERASING:
            /* Erase is synchronous in prv_HandleHeader */
            break;

        case FWUPDATE_RECEIVING:
            prv_HandleReceiving();
            break;

        case FWUPDATE_VERIFYING:
            prv_HandleVerifying();
            break;

        case FWUPDATE_COMMITTING:
            /* Commit is synchronous in prv_HandleVerifying */
            break;

        case FWUPDATE_DONE:
        case FWUPDATE_ERROR:
            /* Terminal states — stay here until reset or abort */
            break;

        default:
            s_state = FWUPDATE_IDLE;
            break;
    }

    return s_state;
}

FwUpdate_State_t FwUpdate_GetState(void)
{
    return s_state;
}

FwUpdate_Error_t FwUpdate_GetError(void)
{
    return s_lastError;
}

void FwUpdate_Abort(void)
{
    if (s_state != FWUPDATE_IDLE && s_state != FWUPDATE_ERROR)
    {
        Debug_Print("[FWUP] Aborted\r\n");
        NvLog_WriteU32(NVLOG_EVT_FWUPDATE_FAIL, NVLOG_SRC_FWUPDATE,
                       NVLOG_SEV_WARNING, (uint32)s_state);
    }
    UartXfer_FlushRx();
    s_state     = FWUPDATE_IDLE;
    s_lastError = FWUPDATE_ERR_NONE;
}