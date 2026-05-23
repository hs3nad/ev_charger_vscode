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
 * Timer 2 ISR  –  ทุก 1 ms
 *   ความสำคัญ (Priority): 4 (ระดับกลาง)
 *   หน้าที่: นับถอยหลังตัวจับเวลาทั้งหมด + ควบคุม buzzer tempo
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

    /* ---- countdown สำหรับ dim หน้าจอ / LED (ลดลงแต่ไม่ set flag) ---- */
    if (msecDim  > 0u) { msecDim--;  }
    if (msecDim2 > 0u) { msecDim2--; }

    /* ---- msecUp1: นับเวลาที่ CP อยู่นอกสถานะชาร์จ
       ใช้ตรวจว่ารถถอดสายจริงๆ หรือแค่ glitch ชั่วคราว
       หยุดนับเมื่อ CP อยู่ในสถานะ Charge หรือ HiTemp ---- */
    if (chargingState < CP_CHARGE || chargingState > CP_HITEMP)
    {
        if (msecUp1 < 0xFFF0u) { msecUp1++; } /* cap ที่ ~65520 ms ป้องกัน overflow */
    }
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

    _T2IF = 0; /* ล้าง interrupt flag (ต้องทำท้ายสุดเสมอ) */
}

/* =========================================================================
 * Timer 1 ISR  –  ทุก 100 µs (10 kHz)
 *   ความสำคัญ: 7 (สูงสุด)
 *   หน้าที่หลัก:
 *     1. สร้าง CP PWM signal (1 kHz) สำหรับสื่อสารกับรถ
 *     2. กระพริบ LED ภายนอกตอนรถเสียบสาย (JOB_CONNECT)
 *
 *   CP PWM ทำงานแบบ software timer:
 *     - 1 period = 10 tick × 100 µs = 1 ms → 1 kHz
 *     - pwmDuty = 50 → High 5 tick, Low 5 tick
 *     - pwmDuty = 40 → High 4 tick, Low 6 tick
 *     - pwmDuty = 60 → High 6 tick, Low 4 tick (สูงสุด)
 *
 *   เหตุผลที่จำกัด duty 40–60%:
 *     ตาม IEC 61851 duty 10%–96% ตรงกับกระแส 6A–80A
 *     เราจำกัดไว้ที่ 40–60% ≈ 20–30A เพื่อความปลอดภัย
 * ========================================================================= */
void __attribute__((interrupt, no_auto_psv)) _T1Interrupt(void)
{
    /* ---- กรณีพิเศษ JOB_STOP: หยุด PWM, CP = +12V (relay เปิดแต่ไม่ชาร์จ) ---- */
    if (jobStep == JOB_STOP)
    {
        PWM_OUT  = 1;          /* CP = +12V (ระดับ "ไม่มีรถ" เพื่อสั่งรถหยุดชาร์จ) */
        cpAdcRaw = cpAdcPrev;  /* อัปเดตค่า CP จาก ADC ด้วย */
        _T1IF    = 0;
        return; /* ออกจาก ISR ทันที ไม่ต้องทำส่วน PWM */
    }

    /* ---- สถานะปกติ: สร้าง CP PWM ---- */
    switch (pwmSubStep)
    {
        case 0: /* ---- ช่วง High ของ PWM ---- */
            if (pwmHighCnt > 0u)
            {
                PWM_OUT = 1; /* ค้าง High ไปก่อน */
                if (pwmHighCnt < 10u)
                {
                    pwmHighCnt--; /* นับถอยหลังช่วง High */
                }
                else
                {
                    /* คำนวณ High/Low count ใหม่จาก pwmDuty */
                    pwmHighCnt = pwmDuty / 10u;
                    if (pwmHighCnt > 7u) { pwmHighCnt = 7u; } /* cap ไม่เกิน 7 */
                    pwmLowCnt  = 10u - pwmHighCnt;
                }
            }
            else
            {
                /* High หมดแล้ว → เปลี่ยนเป็น Low */
                cpAdcRaw   = cpAdcPrev; /* snapshot ค่า ADC ขณะสิ้นสุดช่วง High */
                PWM_OUT    = Flag.CPenb ? 0u : 1u; /* CPenb=1: Low, CPenb=0: ค้าง High */
                pwmSubStep = 1u;
                if (pwmLowCnt > 0u) { pwmLowCnt--; }
            }
            break;

        case 1: /* ---- ช่วง Low ของ PWM ---- */
            if (pwmLowCnt > 0u)
            {
                PWM_OUT = Flag.CPenb ? 0u : 1u;
                pwmLowCnt--;
            }
            else
            {
                /* Low หมดแล้ว → กลับเป็น High เริ่ม period ใหม่ */
                PWM_OUT    = 1;
                pwmSubStep = 0u;
                pwmHighCnt = pwmDuty / 10u;
                if (pwmHighCnt > 7u) { pwmHighCnt = 7u; }
                pwmLowCnt  = 10u - pwmHighCnt;
                if (pwmHighCnt > 0u) { pwmHighCnt--; } /* เริ่มนับทันที */
            }
            break;

        default: /* state ผิดปกติ – reset */
            pwmSubStep = 0u;
            pwmHighCnt = pwmDuty / 10u;
            if (pwmHighCnt > 7u) { pwmHighCnt = 7u; }
            pwmLowCnt  = 10u - pwmHighCnt;
            break;
    }

    /* ---- กระพริบ LED ภายนอกทุก 100 µs ขณะ relay เปิด (JOB_CONNECT) ---- */
    if (jobStep == JOB_CONNECT)
    {
        static uint8_t blinkDiv = 0u;
        blinkDiv++;
        /* สลับ on/off ทุก tick → กระพริบ 5 kHz (มองเห็นเป็นแสงสม่ำเสมอ) */
        if (blinkDiv & 1u) { LEDX_GRN = LEDX_RED = LEDX_BLU = 1; }
        else               { LEDX_GRN = LEDX_RED = LEDX_BLU = 0; }
    }

    _T1IF = 0;
}

/* =========================================================================
 * Timer 3 ISR  –  ทุก ~40 µs ตามค่า PR3 ปัจจุบัน
 *   ความสำคัญ: 7 (สูงสุด)
 *   หน้าที่: ส่งข้อมูลสีไปยัง LED WS2812B ผ่าน bit-bang
 *
 *   การทำงานผ่าน ledStep sequencer:
 *   ┌────────────────┬────────────────────────────────────────────┐
 *   │ ledStep        │ การทำงาน                                   │
 *   ├────────────────┼────────────────────────────────────────────┤
 *   │ 0              │ idle – ไม่ส่งข้อมูล (ISR ปิดอยู่)          │
 *   │ 1–9   (9 tick) │ reset pulse: line Low ยาวพอสำหรับ latch    │
 *   │ 10–17 (8 step) │ ส่งข้อมูล LED 0–7 ทีละดวง (G,R,B ต่อดวง) │
 *   │ 18    (1 tick) │ legacy DL_85us แล้ว line High              │
 *   │ 19–36 (18 tick)│ ค้าง line High ตาม sequencer เดิม           │
 *   │ >36            │ เสร็จแล้ว – ปิด ISR                       │
 *   └────────────────┴────────────────────────────────────────────┘
 *
 *   หมายเหตุ: ISR นี้ call ws2812SendByte() ซึ่งกินเวลาตาม bit-bang
 *   sequence จริง ช่วงส่ง LED จึงยาวกว่า Timer3 period ปกติ และ timing
 *   รายบิตต้องดูจาก disassembly/scope ไม่ใช่คำนวณจาก PR3 อย่างเดียว
 * ========================================================================= */
void __attribute__((interrupt, no_auto_psv)) _T3Interrupt(void)
{
    if (ledStep == 0u)
    {
        /* idle – ไม่ทำอะไร (จะเข้ามานี่ก็ต่อเมื่อ ISR เปิดอยู่) */
        WS2812_DAT = 0;
    }
    else if (ledStep >= 1u && ledStep <= 9u)
    {
        /* reset pulse: ดึง line ต่ำยาวพอ (9 tick × ~40 µs = ~360 µs + overhead) */
        WS2812_DAT = 0;
        ledStep++;
    }
    else if (ledStep >= 10u && ledStep <= 17u)
    {
        /* ส่งข้อมูล 1 LED ต่อ step (ledStep 10 = LED 0, 11 = LED 1, ...) */
        uint8_t idx = ledStep - 10u; /* แปลง ledStep → index ใน ledArray */
        ws2812SendByte(ledArray[idx].g); /* ส่งสีเขียวก่อน (WS2812B order) */
        ws2812SendByte(ledArray[idx].r); /* ส่งสีแดง                         */
        ws2812SendByte(ledArray[idx].b); /* ส่งสีน้ำเงิน                      */
        ledStep++;
    }
    else if (ledStep == 18u)
    {
        /* ตรงกับ case 18 เดิม: DL_85us; WSdat=1; */
        WS_T0L();
        WS2812_DAT = 1;
        ledStep++;
    }
    else if (ledStep > 18u && ledStep <= 36u)
    {
        /* line ยัง High อยู่; case 36 เดิมมี DL_85us ก่อน default ดึงลง */
        if (ledStep == 36u)
        {
            WS_T0L();
        }
        ledStep++;
    }
    else
    {
        /* ส่งข้อมูลครบทุก LED แล้ว – ปิด ISR จนกว่าจะมีการอัปเดตครั้งถัดไป */
        WS2812_DAT = 0;
        ledStep    = 0u;
        _T3IE      = 0; /* ปิด Timer3 interrupt */
    }

    _T3IF = 0;
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
 *   Pattern ที่รู้จัก (ตรวจจาก 3 byte ล่าสุดใน buffer):
 *   ┌────────────────────────┬──────────────────────────────────┐
 *   │ Pattern                │ Event                            │
 *   ├────────────────────────┼──────────────────────────────────┤
 *   │ FF FF FF               │ กลับหน้าหลัก (go-to-home)        │
 *   │ "pg" + '0'–'9'         │ เปลี่ยนหน้า                      │
 *   │ "bR" + '\r'            │ Reset MCU (software reset)       │
 *   │ "b" + digit + '\r'     │ กดปุ่ม b0–b8                    │
 *   │ "t" + '1'–'5' + '\r'  │ แตะ text field t1–t5             │
 *   │ "m" + '0'–'5' + '\r'  │ message event m0–m5              │
 *   │ "p" + digit + digit    │ ค่า preset 2 หลัก                │
 *   │ "q" + 4 bytes          │ ค่าตัวเลข 32 bit                 │
 *   └────────────────────────┴──────────────────────────────────┘
 *
 *   ข้อสำคัญ: ต้องตรวจ rxIdxNextion >= 2 ก่อนเข้าถึง [idx-2]
 *   เพราะถ้า idx = 0 หรือ 1 จะเกิด underflow → undefined behavior
 * ========================================================================= */
void __attribute__((interrupt, auto_psv)) _ISR _U1RXInterrupt(void)
{
    uint16_t rxByte;

    while (U1STAbits.URXDA) /* วนรับทุก byte ที่มีอยู่ใน FIFO */
    {
        if (rxIdxNextion >= (uint16_t)(BUF_NEXTION_LEN - 1u))
        {
            rxIdxNextion = (uint16_t)(BUF_NEXTION_LEN - 2u);
        }

        rxByte = U1RXREG & 0xFFu;
        rxBufNextion[rxIdxNextion]      = (uint8_t)rxByte;
        rxBufNextion[rxIdxNextion + 1u] = 0u; /* null-terminate สำหรับ strcmp */

        /* === Guard: ต้องมี 2 byte ก่อนหน้าก่อนตรวจ pattern === */
        if (rxIdxNextion >= 2u)
        {
            uint8_t b2 = rxBufNextion[rxIdxNextion - 2u]; /* byte ก่อนก่อน */
            uint8_t b1 = rxBufNextion[rxIdxNextion - 1u]; /* byte ก่อน     */
            uint8_t b0 = (uint8_t)rxByte;                 /* byte ปัจจุบัน */

            /* ---- 3×0xFF: กลับหน้าหลัก Nextion ---- */
            if (b2 == 0xFFu && b1 == 0xFFu && b0 == 0xFFu)
            {
                rxIdxNextion = 0u;
                goto u1_exit; /* หยุดประมวลผล byte นี้ทันที */
            }

            /* ---- "pg" + digit: เปลี่ยนหน้า ---- */
            if (b2 == 'p' && b1 == 'g' && b0 >= '0' && b0 <= '9')
            {
                nexPage = b0 - '0'; /* แปลง ASCII digit เป็น int */
                rxBufNextion[rxIdxNextion - 1u] = 0u;
                Flag.PgUdtF = Flag.PgUdtintF = 1;
            }

            /* ---- "q" + 4 byte: ค่าตัวเลข 32 bit จาก Nextion (get command) ---- */
            if (rxIdxNextion >= 4u && rxBufNextion[rxIdxNextion - 4u] >= 'q')
            {
                /* คงลำดับ byte ตาม NxtBf ใน v2.04a */
                nxtValue  = (uint32_t)b0 << 24;
                nxtValue |= (uint32_t)b1 << 16;
                nxtValue |= (uint32_t)b2 << 8;
                nxtValue |= (uint32_t)rxBufNextion[rxIdxNextion - 3u];
                Flag.getF = 1;
            }

            /* ---- "p" + digit + digit: preset value 2 หลัก ---- */
            if (b2 >= 'p' &&
                b1 >= '0' && b1 <= '9' &&
                b0 >= '0' && b0 <= '9')
            {
                atoiBuf[0] = (char)b1; /* หลักสิบ */
                atoiBuf[1] = (char)b0; /* หลักหน่วย */
                Flag.getF  = 1;
            }

            /* ---- command ที่ลงท้ายด้วย '\r' ---- */
            if (b0 == '\r')
            {
                /* "bR\r" = สั่ง reset MCU (ปุ่ม Reset บน Nextion) */
                if (b2 == 'b' && b1 == 'R') { asm("RESET"); }

                /* "b0\r"–"b8\r" = กดปุ่ม */
                if (b2 == 'b' && b1 >= '0' && b1 <= '9')
                {
                    switch (b1)
                    {
                        case '0': Flag.b0F = 1; break; /* บันทึก RFID    */
                        case '1': Flag.b1F = 1; break; /* ลบ RFID ทั้งหมด */
                        case '2': Flag.b2F = 1; break; /* ลด              */
                        case '3': Flag.b3F = 1; break; /* เพิ่ม            */
                        case '5': Flag.b5F = 1; break; /* 1 ชั่วโมง       */
                        case '6': Flag.b6F = 1; break; /* 2 ชั่วโมง       */
                        case '7': Flag.b7F = 1; break; /* 3 ชั่วโมง       */
                        case '8': Flag.b8F = 1; break; /* ไม่จำกัดเวลา    */
                        default:  break;
                    }
                    rxIdxNextion = 0u; /* reset buffer หลังรับ command ครบ */
                }

                /* "t1\r"–"t5\r" = แตะ text field แถว RFID slot */
                if (b2 == 't' && b1 >= '1' && b1 <= '5')
                {
                    switch (b1)
                    {
                        case '1': Flag.t1F = 1; break; /* slot 1 */
                        case '2': Flag.t2F = 1; break; /* slot 2 */
                        case '3': Flag.t3F = 1; break; /* slot 3 */
                        case '4': Flag.t4F = 1; break; /* slot 4 */
                        case '5': Flag.t5F = 1; break; /* slot 5 */
                        default:  break;
                    }
                    rxIdxNextion = 0u;
                }

                /* "m0\r"–"m5\r" = message event */
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
            } /* if b0 == '\r' */
        } /* if rxIdxNextion >= 2 */

        /* เพิ่ม index และป้องกัน overflow */
        rxIdxNextion++;
        rxIdxNextion2++;
        if (rxIdxNextion >= (uint16_t)(BUF_NEXTION_LEN - 1u))
        {
            rxIdxNextion = (uint16_t)(BUF_NEXTION_LEN - 2u); /* เหลือที่ให้ null terminator */
        }
    } /* while URXDA */

u1_exit:
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
 *   หลังเจอ Tag found header, ISR จะจับ byte ต่อไปอีก PN532_TAG_BYTES byte
 *   แล้วตั้ง Flag.PN532GotF ให้ main loop รู้ว่าอ่านบัตรเสร็จแล้ว
 *
 *   Guard: ต้องตรวจ rxIdxRfid >= 4 และ >= 5 ก่อน access [idx-n]
 * ========================================================================= */
void __attribute__((interrupt, auto_psv)) _ISR _U2RXInterrupt(void)
{
    uint16_t rxByte;

    while (U2STAbits.URXDA)
    {
        rxByte = U2RXREG;
        rxBufRfid[rxIdxRfid] = (uint8_t)rxByte;

        /* ---- ถ้า TagFoundF = 1: กำลังรับ UID หลัง header ---- */
        if (Flag.TagFoundF)
        {
            pn532Buf[pn532Count] = (uint8_t)rxByte;
            pn532Count++;
            if (pn532Count > 9u)
            {
                /* รับครบ 10 byte = UID พร้อมให้ main loop ประมวลผล */
                Flag.PN532GotF  = 1;
                Flag.TagFoundF  = 0;
            }
        }

        /* ---- Guard: ต้อง 4 byte ก่อนหน้าสำหรับ Ready ACK pattern ---- */
        if (rxIdxRfid >= 4u)
        {
            /* ตรวจ Ready ACK: 00 FF 00 FF 00 */
            if (rxBufRfid[rxIdxRfid - 4u] == 0x00u &&
                rxBufRfid[rxIdxRfid - 3u] == 0xFFu &&
                rxBufRfid[rxIdxRfid - 2u] == 0x00u &&
                rxBufRfid[rxIdxRfid - 1u] == 0xFFu &&
                rxByte                     == 0x00u)
            {
                Flag.PN532RdyF = 1; /* PN532 พร้อมแล้ว */
                rxIdxRfid      = 0u;
                goto u2_exit;
            }
        }

        /* ---- Guard: ต้อง 5 byte ก่อนหน้าสำหรับ Tag found pattern ---- */
        if (rxIdxRfid >= 5u)
        {
            /* ตรวจ Tag found header: 0C F4 D5 4B 01 01 */
            if (rxBufRfid[rxIdxRfid - 5u] == 0x0Cu &&
                rxBufRfid[rxIdxRfid - 4u] == 0xF4u &&
                rxBufRfid[rxIdxRfid - 3u] == 0xD5u &&
                rxBufRfid[rxIdxRfid - 2u] == 0x4Bu &&
                rxBufRfid[rxIdxRfid - 1u] == 0x01u &&
                rxByte                     == 0x01u)
            {
                /* เจอบัตรแล้ว – เริ่มจับ UID byte */
                Flag.TagFoundF = 1;
                rxIdxRfid      = 0u;
                pn532Count     = 0u;
                continue; /* ไม่เพิ่ม rxIdxRfid (reset แล้ว) */
            }
        }

        /* เพิ่ม index และป้องกัน overflow */
        rxIdxRfid++;
        if (rxIdxRfid >= (uint16_t)(BUF_RFID_LEN - 1u))
        {
            rxIdxRfid = (uint16_t)(BUF_RFID_LEN - 1u);
        }
    } /* while URXDA */

u2_exit:
    _U2RXIF = 0;
}

/* =========================================================================
 * ADC1 ISR  –  เกิดทุกครั้งที่ ADC แปลงเสร็จ
 *   หน้าที่: อ่านค่าและทำ running average สำหรับสัญญาณ CP
 *
 *   อ่านเฉพาะเมื่อ:
 *     - pwmHighCnt = 1 หรือ 2 (ปลายช่วง High ของ PWM – แรงดัน CP เสถียร)
 *     - jobStep = JOB_STOP (CP ค้างที่ +12V ตลอด)
 *
 *   Running average: cpAdcAvg = (cpAdcAvg + sample) / 2
 *   วิธีนี้ให้น้ำหนักกับค่าใหม่ 50% และค่าเก่า 50% (IIR filter อย่างง่าย)
 * ========================================================================= */
void __attribute__((interrupt, auto_psv)) _ISR _ADC1Interrupt(void)
{
    uint16_t sample = ADC1BUF0; /* อ่านค่า ADC จาก result register */

    if ((pwmHighCnt > 0u && pwmHighCnt <= 2u) || jobStep == JOB_STOP)
    {
        /* คำนวณ running average */
        cpAdcAvg  += sample;
        cpAdcPrev  = cpAdcAvg / 2u;
        cpAdcAvg   = cpAdcPrev;
        /* cpAdcPrev จะถูก copy ไปยัง cpAdcRaw ใน T1 ISR */
    }

    _AD1IF = 0;
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
     * Timer 1: tick ทุก 100 µs (10 kHz)
     *   Priority 7 (สูงสุด): ใช้สร้าง CP PWM ที่ต้องการ timing แน่นอน
     *   PR1 = 1600: 1600 × (1/Fcy 16MHz) = 100 µs
     * ===================================================================== */
    T1CON = 0x0000u;  /* Internal clock, prescaler 1:1 */
    PR1   = 1600u;
    TMR1  = 0u;
    _T1IF = 0; _T1IE = 1; _T1IP = 7;
    T1CONbits.TON = 1;

    /* =====================================================================
     * Timer 2: tick ทุก 1 ms (1 kHz)
     *   Priority 4 (กลาง): ใช้นับ software timer ทั้งหมด
     *   T2CON = 0x0020: prescaler 1:64
     *   PR2 = 250: 250 × 64 × (1/Fcy 16MHz) = 1 ms
     * ===================================================================== */
    T2CON = 0x0020u;
    PR2   = 250u;
    TMR2  = 0u;
    _T2IF = 0; _T2IE = 1; _T2IP = 4;
    T2CONbits.TON = 1;

    /* =====================================================================
     * Timer 3: tick ทุก ~40 µs (25 kHz)
     *   Priority 7: ใช้ bit-bang WS2812B LED
     *   เริ่มต้น disable (_T3IE = 0) – เปิดเมื่อต้องส่งข้อมูล LED เท่านั้น
     *   PR3 = 640: 640 × (1/Fcy 16MHz) = 40 µs
     * ===================================================================== */
    T3CON = 0x0000u;
    PR3   = 640u;
    TMR3  = 0u;
    _T3IF = 0; _T3IE = 0; _T3IP = 7; /* ปิด interrupt เริ่มต้น */
    T3CONbits.TON = 1;

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
     *   AD1CON1 = 0x00E0: auto-convert, 12-bit mode
     *   AD1CHS  = 8: เลือก channel AN8
     *   ASAM = 1: auto-sample (เริ่มสุ่มตัวอย่างอัตโนมัติ)
     * ===================================================================== */
    AD1PCFGL = 0xFEFFu;  /* 1111 1110 1111 1111 → AN8 = analog */
    AD1CON1  = 0x00E0u;
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
         *   รอบละ ~200 ms (เพราะ delayMs(200) สำหรับรอค่า PWM จาก Nextion)
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
            /* ---- ตรวจ inactivity: dim หน้าจอเมื่อไม่มีการกด ---- */
            if (rxIdxNextion != rxIdxNextionPrv)
            {
                /* มีข้อมูลจาก Nextion ใหม่ = ผู้ใช้กำลัง interact */
                rxIdxNextionPrv = rxIdxNextion;
                nxtSetBrightness(100u);
                msecDim = 60000u; /* reset countdown 60 วินาที */
            }

            /* ---- แสดงตัวนับครั้งที่ relay ทำงาน ---- */
            sprintf(atoiBuf, "%lu", relayCnt);
            nxtSendText("msg2.txt", atoiBuf);

            /* ====================================================================
             * ประมวลผลบัตร RFID ที่สแกนได้
             *   เข้า block นี้เมื่อ PN532 ISR set Flag.PN532GotF
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
                delayMs(300);          /* รอ PN532 ก่อนส่งคำสั่งถัดไป */
                pn532RequestRead();    /* สั่งสแกนรอบถัดไป */
                buzzTempo = 0u;
                LED_GRN   = 0;
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
                            delayMs(300);
                            pn532RequestRead();
                            buzzTempo = 0u;
                            LED_GRN   = 0;
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
                        for (i = 0u; i < 2u; i++)
                        { buzzTempo=1u; delayMs(60); buzzTempo=0u; delayMs(80); }
                        buzzTempo = 1u; delayMs(320); buzzTempo = 0u;
                    }
                    else
                    {
                        buzzTempo = 1u; delayMs(60);  buzzTempo = 0u; delayMs(80);
                        buzzTempo = 1u; delayMs(320); buzzTempo = 0u;
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
                delayMs(100);
                LED_RED = 1; delayMs(300); LED_RED = 0; /* กระพริบแดง = error */
                Flag.TagFoundF = Flag.PN532GotF = 0;
                pn532Init();
                delayMs(100);
            }

            /* ---- Reinit PN532 อัตโนมัติทุก 60 วินาที ----
               เพื่อป้องกัน PN532 ค้างหรือ miss บัตร ---- */
            if (msecTO2 == 0u)
            {
                msecTO2 = PN532_REINIT_MS;
                pn532Init();
            }

            /* =====================================================================
             * Decode CP signal และแสดงสถานะ
             *   ใช้ atomic snapshot ของ cpAdcRaw เพื่อป้องกัน race condition
             *   (cpAdcRaw อาจถูก ADC ISR เปลี่ยนระหว่างอ่าน)
             * ===================================================================== */
            cpRawSnapshot  = cpAdcRaw;    /* copy แบบ atomic (1 instruction) */
            chargingState  = cpDecodePilot(cpRawSnapshot);
            cpShowStatus(chargingState);  /* แสดงบน Nextion widget msg0 */

            /* แสดงค่า ADC ดิบสำหรับ debug (widget msg1) */
            sprintf(atoiBuf, "%u", (unsigned)cpRawSnapshot);
            uartPutStr1("msg1.txt=\""); uartPutStr1(atoiBuf);
            uartPutChar1('"'); nxtSendTerminator();

            /* รัน state machine ประมวลผล relay และ LED */
            stateMachineRun(chargingState);

            /* =====================================================================
             * อ่านค่า PWM duty ใหม่จาก Nextion slider (j0)
             *   ส่ง "get j0.val" แล้วรอ 200 ms ให้ Nextion ตอบกลับ
             *   U1RX ISR จะเก็บค่าไว้ใน nxtValue
             * ===================================================================== */
            rxIdxNextion = 0u; Flag.getF = 0;
            uartPutStr1("get j0.val"); nxtSendTerminator();
            delayMs(200); /* รอ Nextion ตอบ */

            /* อัปเดต duty ถ้าค่าใหม่อยู่ในช่วงที่ยอมรับ */
            if (nxtValue >= PWM_DUTY_MIN && nxtValue <= PWM_DUTY_MAX)
            {
                pwmDuty = (uint8_t)nxtValue;
            }
            else
            {
                pwmDuty = PWM_DUTY_DEFAULT; /* ค่านอกช่วง → ใช้ default */
            }

            /* บันทึกลง EEPROM เฉพาะเมื่อค่าเปลี่ยนจริง (ป้องกัน write cycle เกิน) */
            if (pwmDutyPrev != pwmDuty)
            {
                pwmDutyPrev = pwmDuty;
                eepromWriteByte(EP_ADDR_PWM_VAL, pwmDuty);
                msecDim = 60000u; /* reset dim countdown เมื่อมีการปรับค่า */
            }

            /* ---- ควบคุมความสว่างหน้าจอ Nextion ----
               dim หลังจากไม่มีกิจกรรม 60 วินาที */
            if (msecDim == 0u) { nxtSetBrightness(15u);  } /* dim: 15% */
            else               { nxtSetBrightness(100u); } /* สว่างเต็ม: 100% */

        } /* while(1) main loop */
    } /* for(;;) outer PN532 reinit loop */

    return 0; /* ไม่มีทางถึงที่นี่ */
}
