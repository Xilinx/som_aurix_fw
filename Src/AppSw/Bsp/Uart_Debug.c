/**
 * @file    Uart_Debug.c
 * @brief   ASCLIN0 interrupt-drained debug UART — iLLD 1.20.0 API.
 *
 * Debug_Print() queues via IfxAsclin_Asc_write() (TIME_INFINITE); only
 * blocks if the ring buffer is full. Never call from ISR context at
 * priority >= UART_TX_ISR_PRIO — the TX ISR couldn't preempt to drain.
 *
 * iLLD 1.20.0 ASCLIN key differences from earlier versions:
 *   - config.pins is IFX_CONST IfxAsclin_Asc_Pins* (pointer, not embedded struct)
 *   - IfxAsclin_Asc_Pins has separate mode fields for each pin
 *   - TX buffer must be at least (dataSize + sizeof(Ifx_Fifo) + 8) bytes
 *   - RX buffer may be NULL_PTR when receive is not used
 */

#include "Uart_Debug.h"
#include "IfxAsclin_Asc.h"
#include "IfxAsclin_PinMap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "IfxCpu_Irq.h"
#include "Ipc.h"

#define TX_DATA_SIZE        4096u
#define FMT_BUF_SIZE        256u
#define UART_TX_ISR_PRIO    5u   /* below ERU_PRIO_THERMTRIP/CARRIER_HOT/
                                  * WD_STROBE (20/21/22) */
#define UART_RX_ISR_PRIO    6u    /* ADD */

/* TX buffer must include Ifx_Fifo header + 8-byte alignment guard. */
static IfxAsclin_Asc s_ascHandle;
static uint8         s_txBuf[TX_DATA_SIZE + sizeof(Ifx_Fifo) + 8u];
static uint8         s_rxBuf[64 + sizeof(Ifx_Fifo) + 8u];    /* ADD */

IFX_INTERRUPT(uartTxISR, 0, UART_TX_ISR_PRIO)
{
    IfxAsclin_Asc_isrTransmit(&s_ascHandle);
}

IFX_INTERRUPT(uartRxISR, 0, UART_RX_ISR_PRIO)
{
    IfxAsclin_Asc_isrReceive(&s_ascHandle);
}

void Debug_Init(void)
{
    IfxAsclin_Asc_Config cfg;

    /* Local const — automatic storage avoids Tasking E306 on extern pin refs. */
    const IfxAsclin_Asc_Pins pins = {
        NULL_PTR,                        IfxPort_InputMode_pullUp,    /* CTS — not used */
        &IfxAsclin0_RXA_P14_1_IN,       IfxPort_InputMode_pullUp,    /* RX  — P14.1    */
        NULL_PTR,                        IfxPort_OutputMode_pushPull, /* RTS — not used */
        &IfxAsclin0_TX_P14_0_OUT,       IfxPort_OutputMode_pushPull, /* TX  — P14.0    */
        IfxPort_PadDriver_cmosAutomotiveSpeed1
    };

    IfxAsclin_Asc_initModuleConfig(&cfg, &MODULE_ASCLIN0);

    cfg.baudrate.baudrate     = (float32)UART_DEBUG_BAUD;
    cfg.baudrate.prescaler    = 1u;
    cfg.baudrate.oversampling = IfxAsclin_OversamplingFactor_16;

    cfg.frame.dataLength = IfxAsclin_DataLength_8;
    cfg.frame.stopBit    = IfxAsclin_StopBit_1;
    cfg.frame.parityBit  = FALSE;

    /* TX interrupt drains the ring buffer in the background; RX/error unused. */
    cfg.interrupt.txPriority    = UART_TX_ISR_PRIO;
    cfg.interrupt.rxPriority    = UART_RX_ISR_PRIO;
    cfg.interrupt.erPriority    = 0u;
    cfg.interrupt.typeOfService = IfxSrc_Tos_cpu0;

    cfg.pins = &pins;

    /* TX software FIFO buffer — must be dataSize + sizeof(Ifx_Fifo) + 8. */
    cfg.txBuffer     = s_txBuf;
    cfg.txBufferSize = (Ifx_SizeT)TX_DATA_SIZE;

    /* RX not used for debug output. */
    cfg.rxBuffer     = s_rxBuf;            
    cfg.rxBufferSize = (Ifx_SizeT)64u; 

    IfxAsclin_Asc_initModule(&s_ascHandle, &cfg);
    IfxCpu_Irq_installInterruptHandler(&uartTxISR, UART_TX_ISR_PRIO);
    IfxCpu_Irq_installInterruptHandler(&uartRxISR, UART_RX_ISR_PRIO);
}

static void prv_RingPut(volatile Ipc_DbgRing_t *r, const char *s)
{
    uint32 h = r->head;
    while (*s != '\0')
    {
        uint32 next = (h + 1u) & (DBGRING_SIZE - 1u);
        if (next == r->tail)
            break;                       /* full: drop, never block */
        r->buf[h] = *s++;
        h = next;
    }
    __dsync();
    r->head = h;
}

void Debug_DrainRings(void)
{
    volatile Ipc_DbgRing_t *rings[2] = { &g_dbgRing1, &g_dbgRing2 };
    uint32 i;
    for (i = 0u; i < 2u; i++)
    {
        uint32 t = rings[i]->tail;
        uint32 h = rings[i]->head;
        while (t != h)
        {
            /* contiguous run up to wrap point, then one Asc write */
            uint32 end = (h > t) ? h : DBGRING_SIZE;
            Ifx_SizeT count = (Ifx_SizeT)(end - t);
            (void)IfxAsclin_Asc_write(&s_ascHandle,
                                      (const void *)&rings[i]->buf[t],
                                      &count, TIME_INFINITE);
            t = (t + (uint32)count) & (DBGRING_SIZE - 1u);
        }
        rings[i]->tail = t;
    }
}

void Debug_Print(const char *str)
{
    if ((str == NULL_PTR) || (*str == '\0'))
        return;

    switch (IfxCpu_getCoreIndex())
    {
        case 1:  prv_RingPut(&g_dbgRing1, str); return;
        case 2:  prv_RingPut(&g_dbgRing2, str); return;
        default: break;                          /* CPU0, CPU3 fall through */
    }

    {
        Ifx_SizeT count = (Ifx_SizeT)strlen(str);
        (void)IfxAsclin_Asc_write(&s_ascHandle, str, &count, TIME_INFINITE);
    }
}

void Debug_Printf(const char *fmt, ...)
{
    char    buf[FMT_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Debug_Print(buf);
}

IfxAsclin_Asc *Debug_GetAscHandle(void)
{
    return &s_ascHandle;
}