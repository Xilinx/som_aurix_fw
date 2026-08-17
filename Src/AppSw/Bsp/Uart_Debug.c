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

#define TX_DATA_SIZE        512u
#define FMT_BUF_SIZE        256u
#define UART_TX_ISR_PRIO    5u   /* below ERU_PRIO_THERMTRIP/CARRIER_HOT/
                                  * WD_STROBE (20/21/22) */

/* TX buffer must include Ifx_Fifo header + 8-byte alignment guard. */
static IfxAsclin_Asc s_ascHandle;
static uint8         s_txBuf[TX_DATA_SIZE + sizeof(Ifx_Fifo) + 8u];

IFX_INTERRUPT(uartTxISR, 0, UART_TX_ISR_PRIO)
{
    IfxAsclin_Asc_isrTransmit(&s_ascHandle);
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
    cfg.interrupt.rxPriority    = 0u;
    cfg.interrupt.erPriority    = 0u;
    cfg.interrupt.typeOfService = IfxSrc_Tos_cpu0;

    cfg.pins = &pins;

    /* TX software FIFO buffer — must be dataSize + sizeof(Ifx_Fifo) + 8. */
    cfg.txBuffer     = s_txBuf;
    cfg.txBufferSize = (Ifx_SizeT)TX_DATA_SIZE;

    /* RX not used for debug output. */
    cfg.rxBuffer     = NULL_PTR;
    cfg.rxBufferSize = 0u;

    IfxAsclin_Asc_initModule(&s_ascHandle, &cfg);
}

void Debug_Print(const char *str)
{
    Ifx_SizeT count;

    if ((str == NULL_PTR) || (*str == '\0'))
    {
        return;
    }
    count = (Ifx_SizeT)strlen(str);
    (void)IfxAsclin_Asc_write(&s_ascHandle, str, &count, TIME_INFINITE);
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
