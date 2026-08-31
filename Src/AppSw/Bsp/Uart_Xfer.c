/**
 * @file    Uart_Xfer.c
 * @brief   Dedicated UART for firmware update data transfer
 *
 * Uses the iLLD ASCLIN/ASC driver for setup, then a lightweight
 * RX ISR that pushes bytes into a ring buffer.  TX is polled
 * (blocking) since responses are short.
 *
 * ASCLIN module selection:
 *   EVAL : ASCLIN1   P15.0 TX / P15.1 RX
 *   SoM  : ASCLIN4   Pins per ARD (update Platform_PinCfg)
 */
/**
 * @file    Uart_Xfer.c
 * @brief   Dedicated UART for firmware update data transfer
 *
 * Interrupt-driven RX with a 4KB ring buffer.  TX is blocking
 * (only used for short ACK/NAK responses).
 *
 * Eval board: ASCLIN1 (P15.0 TX / P15.1 RX)
 * SoM target: ASCLIN4 (P22.6 TXD / P22.5 RXD)
 */

#include "Uart_Xfer.h"
#include "IfxCpu_Irq.h"
#include "Stm_Timer.h"

/* ================================================================== */
/*  ISR priorities                                                    */
/* ================================================================== */

#define UART_XFER_TX_ISR_PRIO   30u
#define UART_XFER_RX_ISR_PRIO   31u
#define UART_XFER_ER_ISR_PRIO   32u

/* ================================================================== */
/*  Ring buffer                                                       */
/* ================================================================== */

//static uint8  s_rxRing[UART_XFER_RX_BUF_SIZE];
static volatile uint32 s_rxHead = 0u;  /* ISR writes here */
static volatile uint32 s_rxTail = 0u;  /* App reads from here */
static IfxAsclin_Asc *s_overrideHandle = NULL_PTR;

#define RX_MASK (UART_XFER_RX_BUF_SIZE - 1u)

/* ================================================================== */
/*  iLLD ASC handle + buffers                                         */
/* ================================================================== */

static IfxAsclin_Asc s_ascHandle;

/* iLLD requires its own TX/RX buffers for the driver internals */
static uint8 s_illdTxBuf[64];
static uint8 s_illdRxBuf[64];

/* ================================================================== */
/*  ISRs                                                              */
/* ================================================================== */

IFX_INTERRUPT(uartXferTxISR, 0, UART_XFER_TX_ISR_PRIO)
{
    IfxAsclin_Asc_isrTransmit(&s_ascHandle);
}

IFX_INTERRUPT(uartXferRxISR, 0, UART_XFER_RX_ISR_PRIO)
{
    IfxAsclin_Asc_isrReceive(&s_ascHandle);
}

IFX_INTERRUPT(uartXferErISR, 0, UART_XFER_ER_ISR_PRIO)
{
    IfxAsclin_Asc_isrError(&s_ascHandle);
}

static IfxAsclin_Asc *prv_GetHandle(void)
{
    return (s_overrideHandle != NULL_PTR) ? s_overrideHandle : &s_ascHandle;
}

/* ================================================================== */
/*  Public API                                                        */
/* ================================================================== */

void UartXfer_Init(void)
{
    IfxAsclin_Asc_Config ascCfg;
    IfxAsclin_Asc_initModuleConfig(&ascCfg,
#if defined(TARGET_EVAL_BOARD)
        &MODULE_ASCLIN1
#else
        &MODULE_ASCLIN4
#endif
    );

    ascCfg.baudrate.baudrate   = (float32)UART_XFER_BAUD;
    ascCfg.baudrate.oversampling = IfxAsclin_OversamplingFactor_16;

    ascCfg.interrupt.txPriority = UART_XFER_TX_ISR_PRIO;
    ascCfg.interrupt.rxPriority = UART_XFER_RX_ISR_PRIO;
    ascCfg.interrupt.erPriority = UART_XFER_ER_ISR_PRIO;
    ascCfg.interrupt.typeOfService = IfxSrc_Tos_cpu0;

    ascCfg.txBuffer     = s_illdTxBuf;
    ascCfg.txBufferSize = sizeof(s_illdTxBuf);
    ascCfg.rxBuffer     = s_illdRxBuf;
    ascCfg.rxBufferSize = sizeof(s_illdRxBuf);

    /* Pin assignment */
#if defined(TARGET_EVAL_BOARD)
    /* ASCLIN1: P15.0 TX, P15.1 RX */
    const IfxAsclin_Asc_Pins pins = {
        NULL_PTR,                            IfxPort_InputMode_pullUp,   /* CTS */
        &IfxAsclin1_RXA_P15_1_IN,           IfxPort_InputMode_pullUp,   /* RX */
        NULL_PTR,                            IfxPort_OutputMode_pushPull, /* RTS */
        &IfxAsclin1_TX_P15_0_OUT,           IfxPort_OutputMode_pushPull, /* TX */
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
#else
    /* ASCLIN4: P22.5 RXD, P22.6 TXD (SoM: dedicated AURIX↔Ryzen UART) */
    const IfxAsclin_Asc_Pins pins = {
        NULL_PTR,                            IfxPort_InputMode_pullUp,
        &IfxAsclin4_RXC_P22_6_IN,           IfxPort_InputMode_pullUp,
        NULL_PTR,                            IfxPort_OutputMode_pushPull,
        &IfxAsclin4_TX_P22_5_OUT,           IfxPort_OutputMode_pushPull,
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };
#endif
    ascCfg.pins = &pins;

    IfxAsclin_Asc_initModule(&s_ascHandle, &ascCfg);

    /* Install ISRs */
    IfxCpu_Irq_installInterruptHandler(&uartXferTxISR, UART_XFER_TX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&uartXferRxISR, UART_XFER_RX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&uartXferErISR, UART_XFER_ER_ISR_PRIO);

    /* Clear ring buffer */
    s_rxHead = 0u;
    s_rxTail = 0u;
}

uint32 UartXfer_Available(void)
{
    return (uint32)IfxAsclin_Asc_getReadCount(prv_GetHandle());
}

uint32 UartXfer_Read(uint8 *pDest, uint32 maxLen)
{
    Ifx_SizeT count = (Ifx_SizeT)maxLen;
    IfxAsclin_Asc_read(prv_GetHandle(), pDest, &count, 0u);
    return (uint32)count;
}

uint32 UartXfer_ReadBlocking(uint8 *pDest, uint32 len, uint32 timeoutMs)
{
    uint32 got = 0u;
    uint32 startMs = Stm_GetTimeMs();
    while (got < len)
    {
        Ifx_SizeT remain = (Ifx_SizeT)(len - got);
        IfxAsclin_Asc_read(prv_GetHandle(), &pDest[got], &remain, 0u);
        got += (uint32)remain;
        if (got < len && timeoutMs > 0u &&
            (Stm_GetTimeMs() - startMs) >= timeoutMs)
            break;
    }
    return got;
}

void UartXfer_Write(const uint8 *pSrc, uint32 len)
{
    Ifx_SizeT count = (Ifx_SizeT)len;
    IfxAsclin_Asc_write(prv_GetHandle(), pSrc, &count, TIME_INFINITE);
}

void UartXfer_FlushRx(void)
{
    uint8 dummy;
    Ifx_SizeT one = 1u;
    while ((uint32)IfxAsclin_Asc_getReadCount(prv_GetHandle()) > 0u)
        IfxAsclin_Asc_read(prv_GetHandle(), &dummy, &one, 0u);
}

void UartXfer_SetHandle(IfxAsclin_Asc *asc)
{
    s_overrideHandle = asc;
}
