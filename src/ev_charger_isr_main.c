/**
 * @file    ev_charger_isr_main.c
 * @brief   Interrupt Service Routines (ISR) และฟังก์ชัน main()
 *          สำหรับ Firmware เครื่องชาร์จรถยนต์ไฟฟ้า
 *
 * กฎความปลอดภัยที่ ISR ทุกตัวต้องปฏิบัติ:
 *   1. ต้องมี guard ตรวจ index ก่อน access array ด้วย offset ลบ
 *      (ป้องกัน buffer underflow ที่ทำให้ crash)
 *   2. ทำงานน้อยที่สุด: set flag, copy byte, clear interrupt flag
 *   3. ห้าม delayMs(), ห้ามเขียน EEPROM, ห้ามเรียกฟังก์ชัน blocking
 *   4. ล้าง interrupt flag ท้ายสุดเสมอ (ไม่ใช่ต้น)
 */

#include "ev_charger.h"

/* =========================================================================
 * CP PWM hardware output
 *   CP PWM ออกที่ RB4/RP4 ผ่าน OC1 โดยใช้ Timer2 เป็น time base 1 kHz
 *   ADC อ่าน CP จาก Timer3 compare trigger ที่วางไว้ในช่วง High ของ PWM
 * ========================================================================= */
#define CP_PWM_TIMER2_PR             249u /* (249+1)×64/Fcy16MHz = 1.000 ms */
#define CP_PWM_TIMER2_PRESCALE        64u
#define CP_PWM_PERIOD_T2_COUNTS      (CP_PWM_TIMER2_PR + 1u)
#define CP_PWM_PERIOD_FCY_TICKS      (CP_PWM_PERIOD_T2_COUNTS * CP_PWM_TIMER2_PRESCALE)
#define CP_PWM_OC_DISABLED   0u
#define CP_PWM_OC_EDGE_PWM   6u
#define CP_PWM_OC_TIMER2     0u
#define CP_PWM_OC_SYNC_TMR2  0x0Cu

#define CP_ADC_TRIGGER_PERIOD_TICKS  CP_PWM_PERIOD_FCY_TICKS
#define CP_ADC_TRIGGER_PR_TICKS      (CP_ADC_TRIGGER_PERIOD_TICKS - 1u)
#define CP_ADC_TRIGGER_OFFSET_TICKS    4800u /* trigger ~300 us หลังเริ่มคาบ PWM */
#define CP_ADC_TRIGGER_PRELOAD_TICKS  (CP_ADC_TRIGGER_PERIOD_TICKS - CP_ADC_TRIGGER_OFFSET_TICKS)
#define CP_ADC_CON1_TIMER3_TRIGGER    0x0040u /* SSRC=010: Timer3 compare starts conversion */

static uint8_t cpPwmHwEnabled = 0xFFu;
static uint8_t cpPwmHwDuty    = 0xFFu;

static uint8_t cpPwmClampDuty(uint8_t duty)
{
    if (duty < PWM_DUTY_MIN) { return PWM_DUTY_MIN; }
    if (duty > PWM_DUTY_MAX) { return PWM_DUTY_MAX; }
    return duty;
}

static uint16_t cpPwmDutyToOcTicks(uint8_t duty)
{
    return (uint16_t)(((uint32_t)CP_PWM_PERIOD_T2_COUNTS * cpPwmClampDuty(duty)) / 100u);
}

static void cpAdcTriggerAlignToPwm(void)
{
    TMR2 = 0u;
    TMR3 = CP_ADC_TRIGGER_PRELOAD_TICKS;
    _T3IF = 0u;
}

static void cpPwmForceHigh(void)
{
    if (cpPwmHwEnabled != 0u)
    {
        OC1CON1bits.OCM = CP_PWM_OC_DISABLED;
        _RP4R = 0u;
        cpPwmHwEnabled = 0u;
        cpPwmHwDuty = 0xFFu;
    }
    PWM_OUT = 1u; /* CP = +12V เมื่อยังไม่อนุญาตให้รถชาร์จ */
}

static void cpPwmEnableHardware(void)
{
    uint8_t duty = cpPwmClampDuty(pwmDuty);
    uint16_t dutyTicks = cpPwmDutyToOcTicks(duty);

    if (cpPwmHwEnabled != 1u)
    {
        OC1CON1bits.OCM = CP_PWM_OC_DISABLED;
        _RP4R = 0u;

        OC1R  = dutyTicks;
        OC1RS = dutyTicks;
        OC1CON1bits.OCTSEL = CP_PWM_OC_TIMER2;
        OC1CON2bits.SYNCSEL = CP_PWM_OC_SYNC_TMR2;
        OC1CON2bits.OCINV = 0u;
        OC1CON2bits.OCTRIS = 0u;

        _RP4R = _RPOUT_OC1; /* RP4/RB4 = OC1 output */
        cpAdcTriggerAlignToPwm();
        OC1CON1bits.OCM = CP_PWM_OC_EDGE_PWM;

        cpPwmHwEnabled = 1u;
        cpPwmHwDuty = duty;
    }
    else if (cpPwmHwDuty != duty)
    {
        OC1R  = dutyTicks;
        OC1RS = dutyTicks;
        cpPwmHwDuty = duty;
    }
}

static void cpPwmService(void)
{
    if (Flag.CPenb != 0u && jobStep != JOB_STOP)
    {
        cpPwmEnableHardware();
    }
    else
    {
        cpPwmForceHigh();
    }
}

static void cpPwmHardwareInit(void)
{
    OC1CON1 = 0u;
    OC1CON2 = 0u;
    OC1R = 0u;
    OC1RS = 0u;
    _OC1IF = 0u;
    _OC1IE = 0u;
    cpPwmForceHigh();
}

/* =========================================================================
 * UART RX queues
 *   ISR ทำแค่รับ byte เข้า queue; parser ทำงานใน main context ผ่าน
 *   evChargerServiceRx() เพื่อลดงานใน interrupt
 * ========================================================================= */
#define UART1_RX_QUEUE_LEN  128u
#define UART2_RX_QUEUE_LEN  256u

static volatile uint8_t  uart1RxQueue[UART1_RX_QUEUE_LEN];
static volatile uint8_t  uart2RxQueue[UART2_RX_QUEUE_LEN];
static volatile uint16_t uart1RxHead = 0u;
static volatile uint16_t uart1RxTail = 0u;
static volatile uint16_t uart2RxHead = 0u;
static volatile uint16_t uart2RxTail = 0u;

static void uart1RxQueuePush(uint8_t value)
{
    uint16_t next = uart1RxHead + 1u;
    if (next >= UART1_RX_QUEUE_LEN) { next = 0u; }
    if (next != uart1RxTail)
    {
        uart1RxQueue[uart1RxHead] = value;
        uart1RxHead = next;
    }
}

static uint8_t uart1RxQueuePop(uint8_t *value)
{
    if (uart1RxTail == uart1RxHead) { return 0u; }
    *value = uart1RxQueue[uart1RxTail];
    uart1RxTail++;
    if (uart1RxTail >= UART1_RX_QUEUE_LEN) { uart1RxTail = 0u; }
    return 1u;
}

static void uart2RxQueuePush(uint8_t value)
{
    uint16_t next = uart2RxHead + 1u;
    if (next >= UART2_RX_QUEUE_LEN) { next = 0u; }
    if (next != uart2RxTail)
    {
        uart2RxQueue[uart2RxHead] = value;
        uart2RxHead = next;
    }
}

static uint8_t uart2RxQueuePop(uint8_t *value)
{
    if (uart2RxTail == uart2RxHead) { return 0u; }
    *value = uart2RxQueue[uart2RxTail];
    uart2RxTail++;
    if (uart2RxTail >= UART2_RX_QUEUE_LEN) { uart2RxTail = 0u; }
    return 1u;
}

void evChargerFlushNextionRx(void)
{
    uint16_t ie = _U1RXIE;

    _U1RXIE = 0u;
    while (U1STAbits.URXDA) { (void)U1RXREG; }
    uart1RxHead = 0u;
    uart1RxTail = 0u;
    _U1RXIF = 0u;
    _U1RXIE = ie;
}

void evChargerFlushRfidRx(void)
{
    uint16_t ie = _U2RXIE;

    _U2RXIE = 0u;
    while (U2STAbits.URXDA) { (void)U2RXREG; }
    uart2RxHead = 0u;
    uart2RxTail = 0u;
    _U2RXIF = 0u;
    _U2RXIE = ie;
}

static void nextionProcessByte(uint8_t rxByte)
{
    if (rxIdxNextion >= (uint16_t)(BUF_NEXTION_LEN - 1u))
    {
        rxIdxNextion = (uint16_t)(BUF_NEXTION_LEN - 2u);
    }

    rxBufNextion[rxIdxNextion]      = rxByte;
    rxBufNextion[rxIdxNextion + 1u] = 0u;

    if (rxIdxNextion >= 2u)
    {
        uint8_t b2 = rxBufNextion[rxIdxNextion - 2u];
        uint8_t b1 = rxBufNextion[rxIdxNextion - 1u];
        uint8_t b0 = rxByte;

        if (b2 == 0xFFu && b1 == 0xFFu && b0 == 0xFFu)
        {
            if (rxIdxNextion >= 7u &&
                rxBufNextion[rxIdxNextion - 7u] == 'q')
            {
                nxtValue  = (uint32_t)rxBufNextion[rxIdxNextion - 3u] << 24;
                nxtValue |= (uint32_t)rxBufNextion[rxIdxNextion - 4u] << 16;
                nxtValue |= (uint32_t)rxBufNextion[rxIdxNextion - 5u] << 8;
                nxtValue |= (uint32_t)rxBufNextion[rxIdxNextion - 6u];
                Flag.getF = 1;
            }
            rxIdxNextion = 0u;
            return;
        }

        if (b2 == 'p' && b1 == 'g' && b0 >= '0' && b0 <= '9')
        {
            nexPage = b0 - '0';
            rxBufNextion[rxIdxNextion - 1u] = 0u;
            Flag.PgUdtF = Flag.PgUdtintF = 1;
        }

        if (b2 == 'p' &&
            b1 >= '0' && b1 <= '9' &&
            b0 >= '0' && b0 <= '9')
        {
            atoiBuf[0] = (char)b1;
            atoiBuf[1] = (char)b0;
            atoiBuf[2] = '\0';
            Flag.getF  = 1;
        }

        if (b0 == '\r')
        {
            if (b2 == 'b' && b1 == 'R') { asm("RESET"); }

            if (b2 == 'b' && b1 >= '0' && b1 <= '9')
            {
                switch (b1)
                {
                    case '0': Flag.b0F = 1; break;
                    case '1': Flag.b1F = 1; break;
                    case '2': Flag.b2F = 1; break;
                    case '3': Flag.b3F = 1; break;
                    case '5': Flag.b5F = 1; break;
                    case '6': Flag.b6F = 1; break;
                    case '7': Flag.b7F = 1; break;
                    case '8': Flag.b8F = 1; break;
                    default:  break;
                }
                rxIdxNextion = 0u;
            }

            if (b2 == 't' && b1 >= '1' && b1 <= '5')
            {
                switch (b1)
                {
                    case '1': Flag.t1F = 1; break;
                    case '2': Flag.t2F = 1; break;
                    case '3': Flag.t3F = 1; break;
                    case '4': Flag.t4F = 1; break;
                    case '5': Flag.t5F = 1; break;
                    default:  break;
                }
                rxIdxNextion = 0u;
            }

            if (b2 == 'm' && b1 >= '0' && b1 <= '5')
            {
                switch (b1)
                {
                    case '0': Flag.m0F = 1; break;
                    case '1': Flag.m1F = 1; break;
                    case '2': Flag.m2F = 1; break;
                    case '3': Flag.m3F = 1; break;
                    case '4': Flag.m4F = 1; break;
                    case '5': Flag.m5F = 1; break;
                    default:  break;
                }
                rxIdxNextion = 0u;
            }
        }
    }

    rxIdxNextion++;
    rxIdxNextion2++;
    if (rxIdxNextion >= (uint16_t)(BUF_NEXTION_LEN - 1u))
    {
        rxIdxNextion = (uint16_t)(BUF_NEXTION_LEN - 2u);
    }
}

static void rfidProcessByte(uint8_t rxByte)
{
    if (rxIdxRfid >= (uint16_t)BUF_RFID_LEN)
    {
        rxIdxRfid = (uint16_t)(BUF_RFID_LEN - 1u);
    }
    rxBufRfid[rxIdxRfid] = rxByte;

    if (Flag.TagFoundF)
    {
        if (pn532Count < PN532_TAG_BYTES)
        {
            pn532Buf[pn532Count] = rxByte;
            pn532Count++;
        }
        if (pn532Count >= PN532_TAG_BYTES)
        {
            Flag.PN532GotF = 1;
            Flag.TagFoundF = 0;
        }
        return;
    }

    if (rxIdxRfid >= 4u)
    {
        if (rxBufRfid[rxIdxRfid - 4u] == 0x00u &&
            rxBufRfid[rxIdxRfid - 3u] == 0xFFu &&
            rxBufRfid[rxIdxRfid - 2u] == 0x00u &&
            rxBufRfid[rxIdxRfid - 1u] == 0xFFu &&
            rxByte                    == 0x00u)
        {
            Flag.PN532RdyF = 1;
            rxIdxRfid = 0u;
            return;
        }
    }

    if (rxIdxRfid >= 5u)
    {
        if (rxBufRfid[rxIdxRfid - 5u] == 0x0Cu &&
            rxBufRfid[rxIdxRfid - 4u] == 0xF4u &&
            rxBufRfid[rxIdxRfid - 3u] == 0xD5u &&
            rxBufRfid[rxIdxRfid - 2u] == 0x4Bu &&
            rxBufRfid[rxIdxRfid - 1u] == 0x01u &&
            rxByte                    == 0x01u)
        {
            Flag.TagFoundF = 1;
            rxIdxRfid = 0u;
            pn532Count = 0u;
            return;
        }
    }

    rxIdxRfid++;
    if (rxIdxRfid >= (uint16_t)(BUF_RFID_LEN - 1u))
    {
        rxIdxRfid = (uint16_t)(BUF_RFID_LEN - 1u);
    }
}

void evChargerServiceRx(void)
{
    uint8_t rxByte;
    uint16_t guard;

    guard = UART1_RX_QUEUE_LEN;
    while (guard > 0u && uart1RxQueuePop(&rxByte))
    {
        nextionProcessByte(rxByte);
        guard--;
    }

    guard = UART2_RX_QUEUE_LEN;
    while (guard > 0u && uart2RxQueuePop(&rxByte))
    {
        rfidProcessByte(rxByte);
        guard--;
    }
}

/* =========================================================================
 * Timer 2 ISR  –  ทุก 1 ms
 *   ความสำคัญ (Priority): 4 (ระดับกลาง)
 *   หน้าที่:
 *     1. นับถอยหลังตัวจับเวลาทั้งหมด
 *     2. ควบคุม buzzer tempo
 *     3. sync สถานะ OC1 CP PWM
 *
 *   ตัวจับเวลาทุกตัวเป็นแบบ countdown: main loop ตั้งค่า, ISR ลดค่าทุก ms
 *   เมื่อถึง 0 → ตั้ง flag บอก main loop
 * ========================================================================= */
void __attribute__((interrupt, no_auto_psv)) _T2Interrupt(void)
{
    /* ---- ตัวจับเวลาหลักสำหรับ delayMs() ---- */
    if (msc   == 0u) { Flag.mscF    = 1; } else { msc--;   }
    if (msc2  == 0u) { Flag.msc2F   = 1; } else { msc2--;  }
    if (mscTO == 0u) { Flag.mscTOF  = 1; } else { mscTO--; }
    if (mscTO2== 0u) { Flag.mscTO2F = 1; } else { mscTO2--;}

    /* ---- ตัวจับเวลา timeout ต่างๆ ---- */
    if (msecTO == 0u) { Flag.msecTOF  = 1; } else { msecTO--;  }
    if (msecTO2== 0u) { Flag.msecTO2F = 1; } else { msecTO2--; } /* PN532 reinit */
    if (msecTO3== 0u) { Flag.msecTO3F = 1; } else { msecTO3--; } /* auth timeout */
    if (msecTO4== 0u) { Flag.msecTO4F = 1; } else { msecTO4--; } /* charge timeout */
    if (msecTO5== 0u) { Flag.msecTO5F = 1; } else { msecTO5--; } /* connect timeout */
    if (msecNextionPoll  > 0u) { msecNextionPoll--;  } /* poll Nextion แบบไม่ block */
    if (msecCpRawUpdate  > 0u) { msecCpRawUpdate--;  } /* update ADC debug เป็นช่วงๆ */
    if (msecRfidReadDelay > 0u) { msecRfidReadDelay--; } /* re-arm PN532 แบบไม่ block */
    if (msecPn532InitDelay > 0u) { msecPn532InitDelay--; } /* PN532 init state machine */
    if (msecAuthBeepDelay > 0u) { msecAuthBeepDelay--; } /* auth beep แบบไม่ block */

    /* ---- countdown สำหรับ dim หน้าจอ / LED (ลดลงแต่ไม่ set flag) ---- */
    if (msecDim  > 0u) { msecDim--;  }
    if (msecDim2 > 0u) { msecDim2--; }

    if (msecUp2 < 0xFFF0u) { msecUp2++; }

    /* ---- ควบคุม buzzer ตาม buzzTempo ----
       buzzTempo = 0  : เงียบ
       buzzTempo = 1  : ดังต่อเนื่อง
       buzzTempo = 10 : หยุด (force off)
       buzzTempo = 11–19 : beep N ครั้ง (N = buzzTempo - 10)
                           แต่ละ beep มีความยาว buzzMs ms ---- */
    if      (buzzTempo == 1u)                     { BUZZER = 1; } /* เปิดเสียง */
    else if (buzzTempo == 0u || buzzTempo == 10u)  { BUZZER = 0; } /* ปิดเสียง */

    if (buzzTempoPrev != buzzTempo)
    {
        /* buzzTempo เพิ่งเปลี่ยน – เริ่ม sequence ใหม่ */
        buzzTempoPrev = buzzTempo;
        if (buzzTempo >= 11u)
        {
            buzzMs2 = buzzMs;              /* โหลด countdown */
            BUZZER  = (BUZZER == 0u) ? 1u : 0u; /* toggle ทันที */
        }
    }
    else if (buzzTempo >= 11u && buzzTempo <= 19u)
    {
        /* อยู่ระหว่าง beep sequence: นับ countdown */
        if (buzzMs2 == 0u || buzzMs2 > 5000u)
        {
            buzzMs2 = buzzMs; /* reset countdown รอ beep ถัดไป */
            if (BUZZER == 1u)
            {
                BUZZER = 0u; /* จบ beep นี้ */
            }
            else
            {
                BUZZER = 1u;     /* เริ่ม beep ใหม่ */
                buzzTempo--;     /* ลด count เหลือ beep น้อยลง 1 */
            }
        }
        else { buzzMs2--; } /* ยังไม่ถึงเวลา: นับต่อ */
    }

    cpPwmService();

    _T2IF = 0; /* ล้าง interrupt flag (ต้องทำท้ายสุดเสมอ) */
}

/* =========================================================================
 * UART1 RX ISR  –  รับข้อมูลจาก Nextion display
 *   ความสำคัญ: 5
 *
 *   Nextion ส่ง event กลับมาในรูปแบบ string + 3×0xFF เช่น:
 *     "b0\r"    → กดปุ่ม b0
 *     "pg3"     → เปลี่ยนไปหน้า 3
 *     0xFF×3    → กลับหน้าหลัก
 *
 *   ISR นี้ทำเฉพาะรับ byte เข้า queue; การ parse pattern ย้ายไป
 *   evChargerServiceRx() ซึ่งถูกเรียกใน main context และระหว่าง delayMs()
 * ========================================================================= */
void __attribute__((interrupt, auto_psv)) _ISR _U1RXInterrupt(void)
{
    while (U1STAbits.URXDA) /* วนรับทุก byte ที่มีอยู่ใน FIFO */
    {
        uart1RxQueuePush((uint8_t)(U1RXREG & 0xFFu));
    } /* while URXDA */

    _U1RXIF = 0; /* ล้าง interrupt flag */
}

/* =========================================================================
 * UART2 RX ISR  –  รับข้อมูลจาก PN532 RFID reader
 *   ความสำคัญ: 7 (สูงสุด – priority สูงกว่า Nextion เพราะ timing critical)
 *
 *   PN532 ส่งข้อมูลกลับมา 2 รูปแบบ:
 *     1. Ready ACK: ...00 FF 00 FF 00 → PN532 พร้อมรับคำสั่งแล้ว
 *     2. Tag found: ...0C F4 D5 4B 01 01 → เจอบัตร, byte ถัดมา = UID
 *
 *   ISR นี้ทำเฉพาะรับ byte เข้า queue; การตรวจ ACK/tag header ย้ายไป
 *   evChargerServiceRx() เพื่อลด latency ของ interrupt
 * ========================================================================= */
void __attribute__((interrupt, auto_psv)) _ISR _U2RXInterrupt(void)
{
    while (U2STAbits.URXDA)
    {
        uart2RxQueuePush((uint8_t)(U2RXREG & 0xFFu));
    } /* while URXDA */

    _U2RXIF = 0;
}

/* =========================================================================
 * ADC1 ISR  –  เกิดทุกครั้งที่ ADC แปลงเสร็จ
 *   หน้าที่: อ่านค่าและทำ running average สำหรับสัญญาณ CP
 *
 *   ADC conversion ถูก trigger ด้วย Timer3 compare:
 *     - Timer3 ถูก align กับ Timer2/OC1 ตอนเริ่มเปิด CP PWM
 *     - trigger ประมาณ 300 us หลังเริ่มคาบ PWM เพื่อให้อยู่ในช่วง High
 *       แม้ duty ต่ำสุด 40% (High ~400 us)
 *
 *   Running average: cpAdcAvg = (cpAdcAvg + sample) / 2
 *   วิธีนี้ให้น้ำหนักกับค่าใหม่ 50% และค่าเก่า 50% (IIR filter อย่างง่าย)
 * ========================================================================= */
void __attribute__((interrupt, auto_psv)) _ISR _ADC1Interrupt(void)
{
    uint16_t sample = ADC1BUF0; /* อ่านค่า ADC จาก result register */

    /* คำนวณ running average */
    cpAdcAvg  += sample;
    cpAdcPrev  = cpAdcAvg / 2u;
    cpAdcAvg   = cpAdcPrev;
    cpAdcRaw   = cpAdcPrev;

    _AD1IF = 0;
}

/* =========================================================================
 * Auth beep sequencer – pattern เสียงแบบเดิม แต่ไม่ block main loop
 * ========================================================================= */
#define AUTH_BEEP_NONE   0u
#define AUTH_BEEP_READY  1u
#define AUTH_BEEP_STOP   2u

static uint8_t authBeepMode = AUTH_BEEP_NONE;
static uint8_t authBeepStep = 0u;

static bool authBeepActive(void)
{
    return (authBeepStep != 0u);
}

static void authBeepStart(uint8_t mode)
{
    authBeepMode = mode;
    authBeepStep = 1u;
    buzzTempo = 1u;
    msecAuthBeepDelay = 60u;
}

static void authBeepService(void)
{
    if (authBeepStep == 0u || msecAuthBeepDelay > 0u)
    {
        return;
    }

    if (authBeepMode == AUTH_BEEP_READY)
    {
        switch (authBeepStep)
        {
            case 1u: buzzTempo = 0u; msecAuthBeepDelay = 80u;  authBeepStep = 2u; break;
            case 2u: buzzTempo = 1u; msecAuthBeepDelay = 60u;  authBeepStep = 3u; break;
            case 3u: buzzTempo = 0u; msecAuthBeepDelay = 80u;  authBeepStep = 4u; break;
            case 4u: buzzTempo = 1u; msecAuthBeepDelay = 320u; authBeepStep = 5u; break;
            default: buzzTempo = 0u; authBeepMode = AUTH_BEEP_NONE; authBeepStep = 0u; break;
        }
    }
    else if (authBeepMode == AUTH_BEEP_STOP)
    {
        switch (authBeepStep)
        {
            case 1u: buzzTempo = 0u; msecAuthBeepDelay = 80u;  authBeepStep = 2u; break;
            case 2u: buzzTempo = 1u; msecAuthBeepDelay = 320u; authBeepStep = 3u; break;
            default: buzzTempo = 0u; authBeepMode = AUTH_BEEP_NONE; authBeepStep = 0u; break;
        }
    }
    else
    {
        buzzTempo = 0u;
        authBeepMode = AUTH_BEEP_NONE;
        authBeepStep = 0u;
    }
}

/* =========================================================================
 * main() – จุดเริ่มต้นของโปรแกรม
 * =========================================================================
 *
 * ขั้นตอนการทำงาน:
 *   1. ตั้งค่า hardware (GPIO, Timer, UART, ADC)
 *   2. Test LED และ buzzer
 *   3. โหลดค่าจาก EEPROM (PWM duty, RFID slots, relay counter)
 *   4. Init PN532
 *   5. วน main loop ไม่มีสิ้นสุด
 * ========================================================================= */
int main(void)
{
    uint8_t  firstRun;       /* ค่าจาก EEPROM ตรวจว่าเคย first-run หรือยัง */
    uint8_t  i;              /* ตัวแปรนับ loop ชั่วคราว                    */
    uint16_t cpRawSnapshot;  /* snapshot ของ cpAdcRaw (ป้องกัน race condition) */
    uint16_t cpRawShown = 0xFFFFu;
    uint32_t relayCntShown = 0xFFFFFFFFUL;
    CpState_t chargingStateShown = CP_ERROR;
    uint8_t screenDimmed = 0xFFu;
    uint8_t pwmPollPending = 0u;
    uint8_t rfidReadRearmPending = 0u;

    /* =====================================================================
     * ตั้งค่า Clock
     *   MCU GA106: FRC × PLL → Fosc ~32 MHz
     *   PIC24 ใช้ instruction cycle Fcy = Fosc/2 → ~16 MHz
     *   CLKDIV = 0: ไม่ใช้ divider เพิ่ม
     *   OSCTUN: fine-tune FRC oscillator ให้ Fosc ใกล้ 32 MHz
     * ===================================================================== */
    CLKDIV = 0u;
#if MCU_VARIANT == 1
    OSCTUN = 1u; /* ปรับ FRC สำหรับ GA106 */
#else
    OSCTUN = 2u; /* ปรับ FRC สำหรับ GB106 */
#endif

    /* =====================================================================
     * ตั้งค่า ขา Analog/Digital
     *   ค่า default ให้ทุกขาเป็น digital ก่อน
     *   จะเปิด analog เฉพาะที่ต้องการ (AN8 สำหรับ CP) ทีหลัง
     * ===================================================================== */
    AD1PCFGL = 0xFFFFu;
    AD1PCFGH = 0xFFFFu;
    AD1PCFG  = 0xFFFFu;

    BUZZER = 0; /* ปิดเสียงก่อนเปิดเครื่อง */

    /* =====================================================================
     * ตั้งค่า GPIO (TRIS = 1:input, 0:output)
     *   ตัวเลขในคอมเมนต์แสดง bit 15 ลงมาถึง bit 0
     * ===================================================================== */
    TRISB = 0xD508u; /* 1101 0101 0000 1000 – ขา B input/output */
    PORTB = LATB = 0xD440u;
    _CN5PUE  = 1; /* pull-up CN5  (ขา EEPROM SDA)   */
    _CN12PUE = 1; /* pull-up CN12 (ขา EEPROM SDA)   */

    TRISC = 0x9000u; /* 1001 0000 0000 0000 */
    ODCC  = 0x2000u; /* 0010 0000 0000 0000 – open-drain สำหรับ RC13 */
    _CN22PUE = 1; /* pull-up สวิตช์ขวา (SW_RIGHT) */
    _CN23PUE = 1; /* pull-up สวิตช์ซ้าย (SW_LEFT)  */

    PORTD = TRISD = 0x0220u;
    ODCD  = 0x0402u;

    TRISE = 0x0090u;
    PORTE = 0xFFFFu;
    _CN62PUE = 1; /* pull-up SW1      */
    _CN63PUE = 1; /* pull-up SW2      */
    _CN65PUE = 1; /* pull-up EEPROM SDA */

    TRISF = 0x0030u;
    _CN17PUE = 1; /* pull-up RF4 (UART3 RX) */
    _CN18PUE = 1; /* pull-up RF5 (UART3 TX) */

    TRISG = 0x0240u;
    _CN8PUE = 1; /* pull-up RG6 (UART1 RX จาก Nextion) */
    _CN9PUE = 1; /* pull-up RG7 (UART1 TX ไป Nextion)   */

    /* =====================================================================
     * Timer 2: tick ทุก 1 ms (1 kHz)
     *   Priority 4 (กลาง): ใช้นับ software timer ทั้งหมด และเป็น time base ของ OC1 CP PWM
     *   T2CON = 0x0020: prescaler 1:64
     *   PR2 = 249: (249+1) × 64 × (1/Fcy 16MHz) = 1.000 ms
     * ===================================================================== */
    T2CON = 0x0020u;
    PR2   = CP_PWM_TIMER2_PR;
    TMR2  = 0u;
    _T2IF = 0; _T2IE = 1; _T2IP = 4;
    T2CONbits.TON = 1;

    /* =====================================================================
     * Timer 3: ADC trigger สำหรับอ่าน CP ช่วง High ของ PWM
     *   ไม่เปิด interrupt; ใช้เฉพาะ compare event เป็น trigger ให้ ADC
     *   PR3 derive จาก Timer2 master period เพื่อไม่ให้ phase drift
     *   preload TMR3 ให้ trigger ครั้งแรกหลัง ~300 us จากจุด align
     * ===================================================================== */
    T3CON = 0x0000u;
    PR3   = CP_ADC_TRIGGER_PR_TICKS;
    TMR3  = CP_ADC_TRIGGER_PRELOAD_TICKS;
    _T3IF = 0; _T3IE = 0;
    T3CONbits.TON = 1;

    cpPwmHardwareInit();

    /* =====================================================================
     * UART1: Nextion display  (9600 baud)
     *   ใช้ PPS (Peripheral Pin Select) กำหนดขา:
     *     RX1 = RP21 = RG6 (รับข้อมูลจาก Nextion)
     *     TX1 = RP26 = RG7 (ส่งคำสั่งไป Nextion)
     *   U1MODE = 0x8008: enable UART, 8N1
     * ===================================================================== */
    U1BRG  = 4000000u / 9600u;  /* = 416, ใช้สูตรเดิมจาก v2.04a */
    U1MODE = 0x8008u;
    U1STAbits.UTXEN = 1;
    _U1RXIF = 0; _U1RXIE = 1; _U1RXIP = 5;
    _U1RXR  = 21u;  /* RP21 (RG6) = รับ UART1 */
    _RP26R  = 3u;   /* RP26 (RG7) = ส่ง UART1 (3 = U1TX) */

    /* =====================================================================
     * UART2: PN532 RFID reader  (115200 baud)
     *   ใช้ priority 7 เพราะ PN532 ส่งข้อมูลเร็วและต้องตอบสนองทันที
     *     RX2 = RP29 = RB15
     *     TX2 = RP9  = RB9
     * ===================================================================== */
    U2BRG  = 4000000u / 115200u;  /* = 34, ใช้สูตรเดิมจาก v2.04a */
    U2MODE = 0x8008u;
    U2STAbits.UTXEN = 1;
    _U2RXIF = 0; _U2RXIE = 1; _U2RXIP = 7;
    _U2RXR  = 29u;  /* RP29 (RB15) = รับ UART2 */
    _RP9R   = 5u;   /* RP9  (RB9)  = ส่ง UART2 (5 = U2TX) */

    /* =====================================================================
     * ADC: อ่านสัญญาณ CP Pilot จาก AN8
     *   AD1PCFGL: ปิด AN8 digital mode (เปิดเป็น analog)
     *   AD1CON1 SSRC=010: Timer3 compare ends sampling and starts conversion
     *   AD1CHS  = 8: เลือก channel AN8
     *   ASAM = 1: auto-sample (เริ่มสุ่มตัวอย่างอัตโนมัติ)
     * ===================================================================== */
    AD1PCFGL = 0xFEFFu;  /* 1111 1110 1111 1111 → AN8 = analog */
    AD1CON1  = CP_ADC_CON1_TIMER3_TRIGGER;
    AD1CON3  = 0x0100u;
    AD1CHS   = 8u;
    _ADON = 1; _AD1IE = 1; _AD1IF = 0; _ASAM = 1;

    RELAY_1 = 0; /* ปิด relay ตั้งต้น */

    /* =====================================================================
     * ตั้งค่าเริ่มต้น LED WS2812B
     *   สีเขียวจาง = standby mode
     * ===================================================================== */
    WS2812_DAT = 0;
    delayMs(100);
    ws2812SetAll(5u, 0u, 0u); /* เขียวจาง */
    delayMs(100);

    /* Nextion: ตั้งค่าสีพื้นหลัง widget สถานะ (สีเทา = idle) */
    uartPutStr1("msg4.bco=61309"); nxtSendTerminator();
    uartPutStr1("vis b0,0");       nxtSendTerminator();
    uartPutStr1("vis b1,0");       nxtSendTerminator();

    /* =====================================================================
     * Power-on test sequence
     *   กระพริบ LED และส่งเสียง 5 ครั้ง ยืนยันว่าวงจรทำงานปกติ
     *   ลำดับ: แดง → เหลือง → เขียว → เหลือง → แดง
     * ===================================================================== */
    i = 60u; /* delay แต่ละขั้น 60 ms */
    buzzTempo = 1u; LED_RED = LEDX_RED = 1; delayMs(i);
    buzzTempo = 0u; delayMs(i); LED_RED = LEDX_RED = 0;

    buzzTempo = 1u; LED_YEL = LEDX_BLU = 1; delayMs(i);
    buzzTempo = 0u; delayMs(i); LED_YEL = LEDX_BLU = 0;

    buzzTempo = 1u; LED_GRN = LEDX_GRN = 1; delayMs(i);
    buzzTempo = 0u; delayMs(i); LED_GRN = LEDX_GRN = 0;
    delayMs(i);

    buzzTempo = 1u; LED_YEL = LEDX_BLU = 1; delayMs(i);
    buzzTempo = 0u; delayMs(i); LED_YEL = LEDX_BLU = 0;

    buzzTempo = 1u; LED_RED = LEDX_RED = 1; delayMs(i);
    buzzTempo = 0u; delayMs(i); LED_RED = LEDX_RED = 0;

    /* Reset flag การ authentication และ button */
    Flag.AuthF = Flag.Auth2F = Flag.Auth3F = 0;
    msecDim = msecDim2 = 60000u;
    Flag.b5F = Flag.b6F = Flag.b7F = Flag.b8F = Flag.m0F = 0;

#if ENABLE_TEST_AUTH_BYPASS
    rfidAuthAdmin = rfidTagId;
    Flag.AuthF = Flag.Auth2F = Flag.Auth3F = 1;
#endif

    /* =====================================================================
     * หมายเหตุ: โค้ด test ที่ line 1218 ของ original (v2.04a)
     *   "IDauth=IDbuf2=IDbuf; Flag.AuthF=Flag.Auth2F=Flag.Auth3F=1;"
     *   ถูกครอบด้วย ENABLE_TEST_AUTH_BYPASS เพราะทำให้ทุกคนชาร์จได้โดยไม่ต้องใช้บัตร
     *   ก่อน deploy ต้องคงค่า ENABLE_TEST_AUTH_BYPASS = 0
     * ===================================================================== */

    /* แสดงเวอร์ชัน firmware บน Nextion 2 วินาที */
    nxtShowVersion();
    delayMs(2000);

    /* =====================================================================
     * โหลดค่า PWM duty จาก EEPROM
     *   ถ้าค่าผิดพลาด (นอกช่วง 40–60) ใช้ค่า default 50%
     * ===================================================================== */
    pwmDuty = eepromReadByte(EP_ADDR_PWM_VAL);
    if (pwmDuty < PWM_DUTY_MIN || pwmDuty > PWM_DUTY_MAX)
    {
        pwmDuty = PWM_DUTY_DEFAULT;
    }
    pwmDutyPrev = pwmDuty;
    sprintf(atoiBuf, "%u", (unsigned)pwmDuty);
    uartPutStr1("j0.val="); uartPutStr1(atoiBuf); nxtSendTerminator();
    nxtSendText("percent.txt", atoiBuf);

    evChargerServiceRx();
    evChargerFlushNextionRx();
    rxIdxNextion = 0u;
    Flag.getF = 0;
    uartPutStr1("get j0.val"); nxtSendTerminator();
    delayMs(200);

    if (Flag.getF)
    {
        if (nxtValue >= PWM_DUTY_MIN && nxtValue <= PWM_DUTY_MAX &&
            pwmDutyPrev != (uint8_t)nxtValue)
        {
            pwmDuty = (uint8_t)nxtValue;
            pwmDutyPrev = pwmDuty;
            eepromWriteByte(EP_ADDR_PWM_VAL, pwmDuty);
        }
    }
    else
    {
        pwmDuty = PWM_DUTY_DEFAULT;
        pwmDutyPrev = PWM_DUTY_DEFAULT;
    }

    sprintf(atoiBuf, "%u", (unsigned)pwmDuty);
    uartPutStr1("j0.val="); uartPutStr1(atoiBuf); nxtSendTerminator();
    nxtSendText("percent.txt", atoiBuf);

    /* เปลี่ยนสี slider ตามค่า duty:
       50% = เขียว (ปกติ), <50% = เหลือง (ต่ำกว่าปกติ), >50% = ส้ม (สูงกว่าปกติ) */
    if (pwmDuty == PWM_DUTY_DEFAULT)
    {
        uartPutStr1("j0.pco=GREEN");  nxtSendTerminator();
        uartPutStr1("t20.bco=GREEN"); nxtSendTerminator();
        uartPutStr1("t20.pco=BLACK"); nxtSendTerminator();
    }
    else if (pwmDuty < PWM_DUTY_DEFAULT)
    {
        uartPutStr1("j0.pco=YELLOW");  nxtSendTerminator();
        uartPutStr1("t20.bco=YELLOW"); nxtSendTerminator();
        uartPutStr1("t20.pco=BLACK");  nxtSendTerminator();
    }
    else
    {
        uartPutStr1("j0.pco=64512");  nxtSendTerminator(); /* สีส้ม */
        uartPutStr1("t20.bco=64512"); nxtSendTerminator();
        uartPutStr1("t20.pco=WHITE"); nxtSendTerminator();
    }

    /* โหลด slot RFID จาก EEPROM และแสดงบน Nextion */
    uartPutStr1("b0.pco=WHITE"); nxtSendTerminator(); delayMs(2);
    uartPutStr1("b1.pco=WHITE"); nxtSendTerminator();
    rfidRefreshDisplay();
    rfidTagId = 0u;

    /* แสดงเวอร์ชันอีกครั้งที่ widget msg1 */
    uartPutStr1("msg1.txt=\""); uartPutStr1(FW_VERSION);
    uartPutChar1('"'); nxtSendTerminator();
    nxtDefaultLayout(); /* โหลด layout ปุ่มปกติ */

    /* =====================================================================
     * ตรวจ First Run
     *   ถ้า EEPROM ยังไม่มีค่า sentinel → reset ตัวนับ relay
     *   ทำเพื่อให้ตัวนับ relay เริ่มจาก 0 บนบอร์ดใหม่
     * ===================================================================== */
    firstRun = eepromReadByte(EP_ADDR_FIRST_RUN);
    if (firstRun != EP_FIRST_RUN_MAGIC)
    {
        /* First run: เขียน sentinel และ reset ตัวนับ */
        eepromWriteLong(EP_ADDR_RELAY_CNT, 0u);
        relayCnt = 0u;
        eepromWriteByte(EP_ADDR_FIRST_RUN, EP_FIRST_RUN_MAGIC);
    }
    else
    {
        /* ไม่ใช่ first run: โหลดตัวนับจาก EEPROM */
        relayCnt = eepromReadLong(EP_ADDR_RELAY_CNT);
    }

    /* =====================================================================
     * Outer loop: วน reinit PN532 เมื่อออกจาก while(1)
     *   ปกติ while(1) ข้างในจะไม่ออก
     *   แต่ถ้า PN532 ค้างหนักมาก for(;;) นี้จะ reinit ให้อัตโนมัติ
     * ===================================================================== */
    msecTO2 = PN532_REINIT_MS; /* เริ่มจับเวลา reinit PN532 ที่ 60 วินาทีเหมือน v2.04a */
    for (;;)
    {
        /* ---- Init PN532 และรีเซ็ต state machine ---- */
        pn532Init();
        jobStep = JOB_IDLE;
        uartPutStr1("msg4.bco=61309"); nxtSendTerminator(); /* Nextion: สีเทา */
        ws2812SetAll(5u, 0u, 0u); /* LED เขียวจาง = standby */
        nxtSetBrightness(100u);

        /* =====================================================================
         * Main while loop
         *   รอบปกติไม่ block: งาน UI/Nextion ทำแบบ on-change หรือ periodic
         *   ทุกรอบ:
         *     1. ตรวจ inactivity → dim/undim หน้าจอ
         *     2. แสดงตัวนับ relay
         *     3. ประมวลผลบัตร RFID (ถ้ามี)
         *     4. Decode CP signal
         *     5. รัน state machine
         *     6. อ่านค่า PWM ใหม่จาก Nextion
         * ===================================================================== */
        msecTO = 2000u;
        while (1)
        {
            evChargerServiceRx();
            pn532InitAsyncService();
            authBeepService();

            if (rfidReadRearmPending && msecRfidReadDelay == 0u && !pn532InitAsyncBusy())
            {
                pn532RequestRead();
                if (!authBeepActive()) { buzzTempo = 0u; }
                LED_GRN   = 0u;
                msecTO    = 1000u;
                rfidReadRearmPending = 0u;
            }

            /* ---- ตรวจ inactivity: dim หน้าจอเมื่อไม่มีการกด ---- */
            if (rxIdxNextion != rxIdxNextionPrv)
            {
                /* มีข้อมูลจาก Nextion ใหม่ = ผู้ใช้กำลัง interact */
                rxIdxNextionPrv = rxIdxNextion;
                if (screenDimmed != 0u)
                {
                    screenDimmed = 0u;
                    nxtSetBrightness(100u);
                }
                msecDim = 60000u; /* reset countdown 60 วินาที */
            }

            /* ---- แสดงตัวนับ relay เฉพาะเมื่อค่าเปลี่ยน ---- */
            if (relayCntShown != relayCnt)
            {
                relayCntShown = relayCnt;
                sprintf(atoiBuf, "%lu", relayCntShown);
                nxtSendText("msg2.txt", atoiBuf);
            }

            /* ====================================================================
             * ประมวลผลบัตร RFID ที่สแกนได้
             *   เข้า block นี้เมื่อ UART RX service set Flag.PN532GotF
             * ==================================================================== */
            if (Flag.PN532GotF)
            {
                msecTO2  = PN532_REINIT_MS; /* รีเซ็ต timeout reinit PN532 */
                LED_GRN  = 1;              /* LED เขียว: อ่านบัตรแล้ว    */
                buzzTempo = 1u;             /* เสียงแจ้งเตือน              */
                msecDim  = 60000u;          /* reset dim countdown         */

                /* แปลง raw bytes → UID uint32_t */
                rfidTagId = pn532ParseTagId();

                msecTO = 1000u;
                Flag.TagFoundF = Flag.PN532GotF = 0; /* ล้าง flag */
                rxIdxRfid = 0u;
                msecRfidReadDelay = RFID_READ_REARM_MS;
                rfidReadRearmPending = 1u; /* สั่งสแกนรอบถัดไปเมื่อ PN532 พร้อม */
                msecTO    = 1000u;

                /* ---- โหมด Admin: บันทึกบัตรใหม่ (ปุ่ม b0 หรือสวิตช์ซ้าย) ---- */
                if (Flag.AuthF && (Flag.b0F || SW_LEFT == 0))
                {
                    Flag.b0F = 0;
                    LED_RED  = 1; /* LED แดง: รอสแกนบัตรที่ต้องการบันทึก */
                    uartPutStr1("b1.pco=BLACK"); nxtSendTerminator();
                    msecTO = 5000u; Flag.msecTOF = 0; /* รอ 5 วินาที */

                    /* วนรอสแกนบัตรที่ต้องการบันทึก */
                    while (!Flag.msecTOF)
                    {
                        evChargerServiceRx();
                        pn532InitAsyncService();
                        authBeepService();

                        if (rfidReadRearmPending && msecRfidReadDelay == 0u && !pn532InitAsyncBusy())
                        {
                            pn532RequestRead();
                            if (!authBeepActive()) { buzzTempo = 0u; }
                            LED_GRN   = 0u;
                            msecTO    = 1000u;
                            rfidReadRearmPending = 0u;
                        }

                        if (Flag.PN532GotF)
                        {
                            msecTO2   = RFID_CHECK_MS;
                            Flag.PN532GotF = 0;
                            LED_GRN   = 1;
                            buzzTempo = 1u;
                            msecDim   = 60000u;
                            rfidTagId = pn532ParseTagId();
                            rfidRecordTag(rfidTagId); /* บันทึกลง EEPROM */
                            rxIdxRfid = 0u;
                            msecRfidReadDelay = RFID_READ_REARM_MS;
                            rfidReadRearmPending = 1u;
                            msecTO    = 1000u;
                        }
                    }
                    LED_RED = 0; /* ปิด LED แดงเมื่อเสร็จหรือหมดเวลา */
                }

                /* ---- ตรวจสิทธิ์บัตร ---- */
                if (rfidIsAuthorised(rfidTagId))
                {
                    /* บัตรผ่าน: ตั้ง flag และ reset timeout */
                    rfidAuthAdmin = rfidTagId;
                    Flag.AuthF = Flag.Auth2F = Flag.Auth3F = 1;
                    msecTO3 = 10000u; /* admin mode timeout 10 วินาที */

                    /* เสียง feedback:
                       - ขณะ idle: 2 beep + beep ยาว = "พร้อมชาร์จ"
                       - ขณะชาร์จ: 1 beep + beep ยาว = "หยุดชาร์จ" */
                    if (jobStep == JOB_IDLE || !Flag.CPenb)
                    {
                        authBeepStart(AUTH_BEEP_READY);
                    }
                    else
                    {
                        authBeepStart(AUTH_BEEP_STOP);
                    }

                    /* แสดงปุ่ม admin บน Nextion */
                    uartPutStr1("vis b0,1"); nxtSendTerminator(); delayMs(1);
                    uartPutStr1("vis b1,1"); nxtSendTerminator(); delayMs(10);
                    uartPutStr1("vis b2,1"); nxtSendTerminator(); delayMs(1);
                    uartPutStr1("vis b3,1"); nxtSendTerminator(); delayMs(10);
                    uartPutStr1("vis b5,1"); nxtSendTerminator(); delayMs(1);
                    uartPutStr1("vis b6,1"); nxtSendTerminator(); delayMs(10);
                    uartPutStr1("vis b7,1"); nxtSendTerminator(); delayMs(1);
                    uartPutStr1("vis b8,1"); nxtSendTerminator(); delayMs(10);
                    uartPutStr1("b0.pco=WHITE"); nxtSendTerminator(); delayMs(2);
                    uartPutStr1("b1.pco=WHITE"); nxtSendTerminator(); delayMs(2);

                    rfidTagId = 0u;
                    Flag.t1F = Flag.t2F = Flag.t3F = Flag.t4F = Flag.t5F = 0;
                }
                else
                {
                    /* บัตรไม่มีสิทธิ์: ซ่อนปุ่ม admin */
                    Flag.AuthF = 0;
                    uartPutStr1("vis b0,0"); nxtSendTerminator(); delayMs(10);
                    uartPutStr1("vis b1,0"); nxtSendTerminator();
                }

                /* ต่ออายุ admin timeout ถ้ามีการกดปุ่ม */
                if ((Flag.b0F || Flag.b1F) && Flag.AuthF) { msecTO3 = 10000u; }

            } /* if PN532GotF */

            /* ---- อัปเดต UI ใน admin mode: ไฮไลต์ slot RFID ที่เลือก ---- */
            if (Flag.AuthF)
            {
                if      (Flag.t1F) { nxtHighlightSelected(1u); }
                else if (Flag.t2F) { nxtHighlightSelected(2u); }
                else if (Flag.t3F) { nxtHighlightSelected(3u); }
                else if (Flag.t4F) { nxtHighlightSelected(4u); }
                else if (Flag.t5F) { nxtHighlightSelected(5u); }

                if (Flag.b0F) { uartPutStr1("b0.pco=BLACK"); nxtSendTerminator(); }
                else if (Flag.b1F || SW_RIGHT == 0)
                {
                    Flag.b1F = 0;
                    uartPutStr1("b1.pco=BLACK"); nxtSendTerminator();
                    buzzTempo = 12u; buzzMs = 300u;
                    rfidEraseAll();
                }
            }

            /* ---- หมดเวลา admin mode: กลับสู่โหมดปกติ ---- */
            if (msecTO3 == 0u && Flag.AuthF)
            {
                msecTO2    = PN532_REINIT_MS;
                Flag.AuthF = 0;
                delayMs(2);
                Flag.b0F = Flag.b1F = 0;
                nxtDefaultLayout(); /* ซ่อนปุ่ม admin, โหลด layout ปกติ */
            }

            /* ---- ป้องกัน PN532 ค้างเพราะบัตรผิดประเภท (เช่น VISA) ----
               ถ้ามีข้อมูลเข้ามาแต่ไม่เจอ tag ที่รู้จัก = น่าจะเป็นบัตร
               ประเภทอื่นที่ PN532 ประมวลผลไม่ได้ → reinit PN532 */
            if (rxIdxRfid == 0u)
            {
                msecTO = 1000u; /* ยังปกติ: reset timeout */
            }
            else if (rxIdxRfid > 0u && !Flag.PN532GotF && msecTO == 0u)
            {
                /* มีข้อมูลแต่ไม่ได้ tag ที่ถูกต้อง หมดเวลาแล้ว → reinit */
                LED_RED = !LED_RED; /* แจ้ง error โดยไม่ block */
                Flag.TagFoundF = Flag.PN532GotF = 0;
                rxIdxRfid = 0u;
                rfidReadRearmPending = 0u;
                msecRfidReadDelay = 0u;
                pn532InitAsyncStart();
                msecTO2 = PN532_REINIT_MS;
            }

            /* ---- Reinit PN532 อัตโนมัติทุก 60 วินาที ----
               เพื่อป้องกัน PN532 ค้างหรือ miss บัตร ---- */
            if (msecTO2 == 0u)
            {
                msecTO2 = PN532_REINIT_MS;
                if (!pn532InitAsyncBusy())
                {
                    rfidReadRearmPending = 0u;
                    msecRfidReadDelay = 0u;
                    pn532InitAsyncStart();
                }
            }

            /* =====================================================================
             * Decode CP signal และแสดงสถานะ
             *   ใช้ atomic snapshot ของ cpAdcRaw เพื่อป้องกัน race condition
             *   (cpAdcRaw อาจถูก ADC ISR เปลี่ยนระหว่างอ่าน)
             * ===================================================================== */
            cpRawSnapshot  = cpAdcRaw;    /* copy แบบ atomic (1 instruction) */
            chargingState  = cpDecodePilot(cpRawSnapshot);
            if (chargingStateShown != chargingState)
            {
                chargingStateShown = chargingState;
                cpShowStatus(chargingState);  /* แสดงบน Nextion widget msg0 */
            }

            /* แสดงค่า ADC ดิบสำหรับ debug เป็นช่วงๆ ไม่ยิงทุก loop */
            if (msecCpRawUpdate == 0u && cpRawShown != cpRawSnapshot)
            {
                cpRawShown = cpRawSnapshot;
                msecCpRawUpdate = CP_RAW_UPDATE_MS;
                sprintf(atoiBuf, "%u", (unsigned)cpRawSnapshot);
                uartPutStr1("msg1.txt=\""); uartPutStr1(atoiBuf);
                uartPutChar1('"'); nxtSendTerminator();
            }

            /* รัน state machine ประมวลผล relay และ LED */
            stateMachineRun(chargingState);

            /* =====================================================================
             * อ่านค่า PWM duty ใหม่จาก Nextion slider (j0) แบบไม่ block
             *   ส่ง get เป็นช่วงๆ แล้วให้ UART RX service เก็บค่าไว้ใน nxtValue
             * ===================================================================== */
            evChargerServiceRx();

            if (Flag.getF)
            {
                Flag.getF = 0;
                pwmPollPending = 0u;
                if (nxtValue >= PWM_DUTY_MIN && nxtValue <= PWM_DUTY_MAX)
                {
                    pwmDuty = (uint8_t)nxtValue;
                }
            }
            else if (msecNextionPoll == 0u)
            {
                if (pwmPollPending == 0u)
                {
                    evChargerFlushNextionRx();
                    rxIdxNextion = 0u;
                    Flag.getF = 0u;
                    uartPutStr1("get j0.val"); nxtSendTerminator();
                    pwmPollPending = 1u;
                }
                else
                {
                    pwmPollPending = 0u; /* timeout: รอบนี้ไม่มีคำตอบ ให้ลองใหม่ภายหลัง */
                }
                msecNextionPoll = NEXTION_PWM_POLL_MS;
            }

            /* บันทึกลง EEPROM เฉพาะเมื่อค่าเปลี่ยนจริง (ป้องกัน write cycle เกิน) */
            if (pwmDutyPrev != pwmDuty)
            {
                pwmDutyPrev = pwmDuty;
                eepromWriteByte(EP_ADDR_PWM_VAL, pwmDuty);
                msecDim = 60000u; /* reset dim countdown เมื่อมีการปรับค่า */
            }

            /* ---- ควบคุมความสว่างหน้าจอ Nextion เฉพาะเมื่อ state dim เปลี่ยน ---- */
            if (msecDim == 0u)
            {
                if (screenDimmed != 1u)
                {
                    screenDimmed = 1u;
                    nxtSetBrightness(15u);
                }
            }
            else if (screenDimmed != 0u)
            {
                screenDimmed = 0u;
                nxtSetBrightness(100u);
            }

        } /* while(1) main loop */
    } /* for(;;) outer PN532 reinit loop */

    return 0; /* ไม่มีทางถึงที่นี่ */
}
