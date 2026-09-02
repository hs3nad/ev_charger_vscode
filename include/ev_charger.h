/**
 * @file    ev_charger.h
 * @brief   Firmware สำหรับเครื่องชาร์จรถยนต์ไฟฟ้า (EV Charger)
 * @board   PCB DIPROM EVSE 1.0  (PIC24FJ128GA106 / GB106)
 * @version v2.04  (ปรับโครงสร้างโค้ดใหม่)
 *
 * ประวัติการแก้ไข
 *   v2.04a – รุ่นแรกที่ออกใช้งาน
 *   v2.04r – ปรับโครงสร้างโค้ดทั้งหมด:
 *            · เปลี่ยนตัวเลขมหัศจรรย์เป็น #define ที่มีชื่อชัดเจน
 *            · ตั้งชื่อตัวแปรให้สื่อความหมาย (เลิกใช้ i, j, k ฯลฯ)
 *            · เพิ่ม enum สำหรับสถานะ state machine
 *            · แยก WS2812B driver ออกเป็น function เดียว (ลดโค้ดซ้ำ)
 *            · เพิ่ม guard ป้องกัน buffer underflow ใน ISR ทุกตัว
 *            · แก้บัก TagAdmin[3] ที่มีค่าเกิน uint32_t
 *            · ลบโค้ด test ที่ข้าม RFID authentication ออก (production)
 */

#ifndef EV_CHARGER_H
#define EV_CHARGER_H

/* =========================================================================
 * เลือกชิป MCU
 *   1 = PIC24FJ128GA106  (บอร์ดปัจจุบัน)
 *   2 = PIC24FJ128GB106  (บอร์ดสำรอง รองรับ USB)
 * ========================================================================= */
#ifndef MCU_VARIANT
#define MCU_VARIANT  1
#endif

#if MCU_VARIANT == 1
  #include <p24FJ128GA106.h>
#else
  #include <p24FJ128GB106.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "tahoma font.h"

#ifndef asm
#define asm __asm__
#endif

/* =========================================================================
 * นิยามชนิดข้อมูลย่อ (เพื่อพิมพ์สั้นลง)
 * ========================================================================= */
typedef unsigned char  uint8_t;   /* จำนวนเต็มไม่มีเครื่องหมาย 8 บิต  */
typedef unsigned int   uint16_t;  /* จำนวนเต็มไม่มีเครื่องหมาย 16 บิต */
typedef unsigned long  uint32_t;  /* จำนวนเต็มไม่มีเครื่องหมาย 32 บิต */

/* =========================================================================
 * ข้อมูลระบุตัว Firmware
 * ========================================================================= */
#define FW_VERSION      "v2.04"   /* เวอร์ชันที่แสดงบนหน้าจอ Nextion  */
#define FW_SOFT_ID      2         /* รหัส software สำหรับระบุรุ่น      */
#define FW_DEVICE_ID    "01"      /* รหัสอุปกรณ์ (Device ID)           */
#define ENABLE_TEST_AUTH_BYPASS 0 /* 1 = เปิด test bypass ตาม v2.04a line 1218 */

/* =========================================================================
 * ค่าคงที่เกี่ยวกับเวลา (หน่วยเป็น มิลลิวินาที นอกจากจะระบุไว้)
 * ========================================================================= */
#define AUTO_OFF_MS         25000u  /* หยุดการทำงานอัตโนมัติ หลังจากไม่มีการกดใดๆ (25 วินาที) */
#define RFID_CHECK_MS       20000u  /* หมดเวลารอสแกน RFID ซ้ำ (20 วินาที)                      */
#define SCREEN_DIM_VAL      "15"    /* ความสว่างหน้าจอ Nextion ตอน dim (15%)                    */
#define PN532_REINIT_MS     60000u  /* รีสตาร์ท PN532 ทุกๆ (60 วินาที) เพื่อป้องกันค้าง        */
#define CHARGE_TIMEOUT_MS    5000u  /* หมดเวลารอสัญญาณชาร์จหลังเสียบสายแล้ว (5 วินาที)        */
#define CONNECT_TIMEOUT_MS  30000u  /* หมดเวลารอรถพร้อมชาร์จ หลังรถเสียบสาย (30 วินาที)       */
#define NEXTION_PWM_POLL_MS   200u  /* รอบถามค่า slider PWM แบบไม่ block main loop            */
#define CP_RAW_UPDATE_MS      500u  /* รอบแสดงค่า ADC debug บน Nextion                        */
#define RFID_READ_REARM_MS    300u  /* รอ PN532 พร้อมก่อนสั่งอ่านบัตรรอบถัดไป                 */
#define PN532_INIT_PRE_MS     500u  /* เว้นช่วงก่อนส่ง init command ให้ PN532                 */
#define PN532_INIT_ACK_MS      10u  /* เว้นช่วงอ่าน ACK หลังส่ง init command                  */
#define PN532_INIT_SAM_MS    1200u  /* รอ SAM configuration เสร็จตามพฤติกรรมเดิม              */
#define PN532_INIT_READ_MS    200u  /* เว้นช่วงหลังส่ง read command ตอน init                  */

/* =========================================================================
 * แผนที่ที่อยู่ EEPROM
 *   EEPROM ภายนอก เชื่อมต่อผ่าน I2C แบบ bit-bang
 *   แต่ละที่อยู่เก็บข้อมูลคนละขนาด (byte / word / long ตามที่ระบุ)
 * =========================================================================
 *   ที่อยู่  ขนาด   เนื้อหา
 *   -------  -----  ----------------------------------------
 *   10       1 byte  ค่า duty cycle PWM (40–60)
 *   20       1 byte  เวลาชาร์จที่เลือกไว้ (1/2/3/>4 ชั่วโมง)
 *   30       2 byte  ค่า CP float reference (สำรอง, ยังไม่ใช้)
 *   44       1 byte  sentinel บอกว่าเคย first-run แล้ว
 *   50       4 byte  จำนวนครั้งที่ relay เคยทำงาน (ตลอดชีวิต)
 *   100-116  4 byte  UID ของบัตร RFID ที่บันทึกไว้ (5 slot)
 * ========================================================================= */
#define EP_ADDR_PWM_VAL      10u    /* ที่อยู่เก็บค่า duty PWM              */
#define EP_ADDR_CHARGE_TIME  20u    /* ที่อยู่เก็บเวลาชาร์จที่ตั้งไว้       */
#define EP_ADDR_CP_REF       30u    /* ที่อยู่เก็บ CP float reference       */
#define EP_ADDR_FIRST_RUN    44u    /* ที่อยู่ sentinel ตรวจ first-run      */
#define EP_FIRST_RUN_MAGIC  123u    /* ค่าที่เขียนลงไปตอน first-run         */
#define EP_ADDR_RELAY_CNT    50u    /* ที่อยู่เก็บตัวนับจำนวนครั้ง relay    */
#define EP_ADDR_RFID_BASE   100u    /* ที่อยู่เริ่มต้นของ slot RFID แรก     */
#define EP_RFID_SLOT_STRIDE   4u    /* ระยะห่างระหว่าง slot (uint32 = 4 byte) */
#define EP_RFID_SLOT_COUNT    5u    /* จำนวน slot บัตร RFID ที่เก็บได้       */

/* =========================================================================
 * ค่า threshold ของ ADC สำหรับสัญญาณ CP (Control Pilot)
 *
 *   CP คือสัญญาณ PWM ±12 V ที่ใช้สื่อสารระหว่างตู้ชาร์จและรถ
 *   ตามมาตรฐาน IEC 61851-1 (SAE J1772)
 *
 *   เอกสารวงจร CP ของบอร์ดนี้คำนวณแรงดันที่ขา ADC โดยประมาณ:
 *     +12V → 3.6V, +9V → 3.2V, +6V → 2.8V, +3V → 2.4V, -12V → 0.6V
 *
 *   ค่า ADC (0–1023) ที่ใช้อยู่เป็น threshold legacy จาก firmware เดิม:
 *   ┌────────────────────────────────────────────────────────┐
 *   │  ADC > 814   →  ไม่มีรถ (floating / state A)          │
 *   │  814 ≥ ADC > 750 →  รถเสียบแต่ยังไม่พร้อม (state B)  │
 *   │  750 ≥ ADC > 672 →  รถพร้อมชาร์จ (state C)           │
 *   │  672 ≥ ADC > 440 →  รถชาร์จ + พัดลม (state C2)       │
 *   │  ADC ≤ 440   →  ผิดปกติ / ลัดวงจร (state E/F)        │
 *   └────────────────────────────────────────────────────────┘
 *   หมายเหตุ: ค่า ADC จริงขึ้นกับ Vref/AVDD และ tolerance ของวงจร
 *             ต้องยืนยันด้วยการวัดที่บอร์ดจริงก่อนปรับ threshold
 * ========================================================================= */
#define CP_THRESH_FLOAT    814u  /* ADC > ค่านี้ = ไม่มีรถเสียบ              */
#define CP_THRESH_CONNECT  750u  /* ADC > ค่านี้ = รถเสียบแล้ว ยังไม่ชาร์จ  */
#define CP_THRESH_CHARGE   672u  /* ADC > ค่านี้ = รถพร้อมชาร์จ              */
#define CP_THRESH_HITEMP   440u  /* ADC > ค่านี้ = รถชาร์จแบบใช้พัดลม       */
/* ADC ≤ CP_THRESH_HITEMP = ผิดปกติ (state E) */

/* =========================================================================
 * ค่า duty cycle PWM สำหรับสัญญาณ CP
 *   duty เก็บในหน่วย % โดยตรง (เช่น 50 = 50%)
 *   ตาม IEC 61851 Annex A ช่วง 10–85% ใช้ Imax = duty × 0.6 A
 *   ดังนั้น 40–60% เท่ากับสั่งกระแสสูงสุดประมาณ 24–36 A
 *   หมายเหตุ: ค่า default 50% คงพฤติกรรมเดิมไว้ แต่เทียบ IEC คือ 30 A
 * ========================================================================= */
#define PWM_DUTY_MIN      40u   /* duty cycle ต่ำสุดที่อนุญาต (40%)    */
#define PWM_DUTY_MAX      60u   /* duty cycle สูงสุดที่อนุญาต (60%)    */
#define PWM_DUTY_DEFAULT  50u   /* duty cycle ตั้งต้น (50% = 30 A ตาม IEC) */

/* =========================================================================
 * ค่าคงที่ RFID / PN532
 * ========================================================================= */
#define PN532_INIT_LEN    28u  /* ขนาด command ตั้งค่า PN532 (byte)           */
#define PN532_READ_LEN    11u  /* ขนาด command สั่งอ่านบัตร (byte)            */
#define PN532_TAG_BYTES   10u  /* จำนวน byte ที่จับหลังเจอ tag header          */
#define RFID_ID_OFFSET     4u  /* ตำแหน่ง byte เริ่มต้นของ UID 4 byte ใน buf */

/**
 * รหัส RFID สำหรับ admin (บัตรพิเศษสำหรับเข้าโหมดตั้งค่าพารามิเตอร์)
 * ข้อสำคัญ: ทุกค่าต้องอยู่ในช่วง uint32_t (ไม่เกิน 4,294,967,295)
 * ค่าเดิม TAG_ADMIN_3 = 20036383907 เกิน 32 บิต → แก้ไขแล้ว
 */
#define TAG_ADMIN_0   2000287835UL  /* บัตร admin หมายเลข 1 */
#define TAG_ADMIN_1   2009918299UL  /* บัตร admin หมายเลข 2 */
#define TAG_ADMIN_2   2011438187UL  /* บัตร admin หมายเลข 3 */
#define TAG_ADMIN_3   2003638390UL  /* บัตร admin หมายเลข 4 (แก้ไขจาก 20036383907 ที่เกิน 32 bit) */

/* =========================================================================
 * ขนาด buffer รับข้อมูล UART (ต้องพอรับ packet ที่ยาวที่สุด)
 * ========================================================================= */
#define BUF_NEXTION_LEN   251u  /* buffer รับข้อมูลจาก Nextion display (UART1) */
#define BUF_RFID_LEN     1021u  /* buffer รับข้อมูลจาก PN532 RFID reader (UART2) */
#define BUF_LORA_LEN      501u  /* buffer รับข้อมูลจาก LoRa module (UART3)      */
#define BUF_AUX_LEN       501u  /* buffer รับข้อมูล UART4 (สำรอง)               */
#define BUF_DISPLAY_LEN   100u  /* buffer สำหรับสร้างข้อความแสดงผล              */
#define BUF_ATOI_LEN       20u  /* buffer สำหรับแปลงตัวเลขเป็น string           */

/* =========================================================================
 * ชื่อแทน GPIO Pin
 *   ใช้ชื่อที่สื่อความหมายแทน _LATxy ดิบๆ เพื่ออ่านโค้ดง่ายขึ้น
 * ========================================================================= */

/* I2C แบบ bit-bang (EEPROM ภายนอก และ RTC ใช้ขาเดียวกัน) */
#define I2C_SDA_TRIS  _TRISB3  /* ทิศทางขา SDA (0=output, 1=input)  */
#define I2C_SDA_IN    _RB3     /* อ่านสถานะ SDA                      */
#define I2C_SDA_OUT   _LATB3   /* เขียนสถานะ SDA                     */
#define I2C_SCL       _LATB5   /* สัญญาณ clock I2C                   */

/* LED แสดงสถานะบนบอร์ด (on-board) */
#define LED_RED   _LATB2  /* LED สีแดง  – ใช้แสดงข้อผิดพลาด / บันทึก */
#define LED_YEL   _LATB1  /* LED สีเหลือง – ใช้แสดงสถานะเชื่อมต่อ    */
#define LED_GRN   _LATB0  /* LED สีเขียว – ใช้แสดงสถานะปกติ           */

/* LED RGB ภายนอก (ติดตั้งที่ตัวเครื่อง) */
#define LEDX_GRN  _LATD10  /* LED RGB ภายนอก ช่องสีเขียว */
#define LEDX_RED  _LATC13  /* LED RGB ภายนอก ช่องสีแดง   */
#define LEDX_BLU  _LATD1   /* LED RGB ภายนอก ช่องสีน้ำเงิน */

/* อุปกรณ์อื่นๆ */
#define LED_IR     _LATF1   /* LED อินฟราเรด (สำหรับเซนเซอร์ IR)          */
#define IR_SENSE   _RB10    /* ขารับสัญญาณเซนเซอร์ IR (AN10)               */
#define BUZZER     _LATG8   /* กริ่ง/บัซเซอร์ (1=ดัง, 0=เงียบ)            */
#define SW_LEFT    _RC12    /* สวิตช์ซ้าย CN23 (0=กด, 1=ปล่อย)            */
#define SW_RIGHT   _RC15    /* สวิตช์ขวา CN22 (0=กด, 1=ปล่อย)             */
#define RELAY_1    _LATB6   /* relay หลัก ควบคุมกระแสชาร์จ (1=ต่อวงจร)    */
#define EXT_SW     _RB4     /* RB4/RP4 ใช้ร่วมกับ CP PWM; ไม่ควรอ่านเป็นสวิตช์เมื่อ OC1 ทำงาน */
#define PWM_OUT    _LATB4   /* latch ของขา CP; PWM จริงออกผ่าน OC1/RP4     */
#define WS2812_DAT _LATF3   /* ขาส่งข้อมูล LED WS2812B (NeoPixel)         */

/* =========================================================================
 * Macro ควบคุมเวลาสำหรับการส่งบิตไปยัง WS2812B
 *   ยึดจำนวน NOP ตามผล preprocess ของ firmware v2.04a:
 *     DL_8us  -> 8 NOP  (ใช้ต่อช่วง High ของบิต 1)
 *     DL_85us -> 8 NOP  (ใช้ต่อช่วง Low ของบิต 0 และ step 18/36)
 *
 *   ชื่อ macro เดิมอ้างอิง timing เชิงตั้งใจของผู้เขียน แต่เวลาจริงต้องรวม
 *   instruction อื่นรอบๆ ด้วย เช่น bset/bclr, bit test, branch และ loop
 *   overhead จึงไม่ควรตีความว่า 8us/0.8us จากชื่อ macro โดยตรง
 *
 *   ที่ Fcy ≈ 16 MHz: 1 instruction cycle ≈ 62.5 ns เฉพาะ instruction
 *   cycle เดี่ยวเท่านั้น หากเปลี่ยน compiler optimization, clock, หรือรูป
 *   โค้ดรอบ macro ต้องตรวจซ้ำจาก disassembly และวัดด้วย oscilloscope
 * ========================================================================= */
#define WS_T1H()  do { \
    asm("NOP");asm("NOP");asm("NOP");asm("NOP"); \
    asm("NOP");asm("NOP");asm("NOP");asm("NOP"); \
} while(0)  /* legacy DL_8us: 8 NOP */

#define WS_T0H()  /* legacy DL_4us: ไม่มี NOP เพิ่ม */

#define WS_T0L()  do { \
    asm("NOP");asm("NOP");asm("NOP");asm("NOP"); \
    asm("NOP");asm("NOP");asm("NOP");asm("NOP"); \
} while(0)  /* legacy DL_85us: 8 NOP */

#define WS_T1L()  /* legacy DL_45us: ไม่มี NOP เพิ่ม */

/* =========================================================================
 * State machine ของกระบวนการชาร์จ (JobStep)
 *
 *   ไดอะแกรม:
 *   [JOB_IDLE] ---(แตะบัตร + CP ไม่ใช่ Float)--> [JOB_CONNECT]
 *   [JOB_CONNECT] ---(CP = Charge/HiTemp)-------> [JOB_CHARGE]
 *   [JOB_CONNECT] ---(timeout)-------------------> [JOB_IDLE]
 *   [JOB_CHARGE]  ---(แตะบัตรหยุด)-------------> [JOB_STOP]
 *   [JOB_CHARGE]  ---(CP = Connect)-------------> [JOB_CONNECT]
 *   [JOB_CHARGE]  ---(CP = Float/Error)---------> [JOB_STOP]
 *   [JOB_STOP]    ---(relay ปิดแล้ว)------------> [JOB_IDLE]
 *   [JOB_STOP]    ---(แตะบัตรเริ่มใหม่)---------> [JOB_CONNECT]
 * ========================================================================= */
typedef enum {
    JOB_IDLE    = 0,  /* ไม่มีรถ / รอสแกนบัตร – relay ปิด, CP = +12V   */
    JOB_CONNECT = 1,  /* รถเสียบสายแล้ว – relay ปิด, CP PWM รอรถพร้อม */
    JOB_CHARGE  = 2,  /* กำลังชาร์จอยู่ – relay เปิด, CP ส่ง PWM       */
    JOB_STOP    = 3   /* หยุดชาร์จ (แตะบัตร) – relay กำลังปิด, CP หยุด */
} JobStep_t;

/* =========================================================================
 * สถานะของสัญญาณ CP ที่อ่านได้จาก ADC
 *   แปลงจากค่า ADC ดิบมาเป็น enum ด้วยฟังก์ชัน cpDecodePilot()
 * ========================================================================= */
typedef enum {
    CP_FLOAT   = 0,  /* ไม่มีรถเสียบ – CP ลอยที่ +12V (state A)         */
    CP_CONNECT = 1,  /* รถเสียบแต่ยังไม่พร้อมชาร์จ (state B, ≈ +9V)     */
    CP_CHARGE  = 2,  /* รถพร้อมชาร์จ (state C, ≈ +6V)                    */
    CP_HITEMP  = 3,  /* รถชาร์จและต้องการพัดลมระบายความร้อน (state C2)   */
    CP_ERROR   = 4   /* ผิดปกติ – อาจลัดวงจรหรือเกิด fault (state E/F)   */
} CpState_t;

/* =========================================================================
 * โครงสร้างข้อมูลสี RGB สำหรับ LED WS2812B
 *   หมายเหตุ: WS2812B รับข้อมูลในลำดับ G → R → B (ไม่ใช่ RGB!)
 * ========================================================================= */
typedef struct {
    uint8_t g;  /* ความเข้มสีเขียว (0–255) */
    uint8_t r;  /* ความเข้มสีแดง   (0–255) */
    uint8_t b;  /* ความเข้มสีน้ำเงิน (0–255) */
} LedRgb_t;

#define LED_COUNT  8u  /* จำนวน LED WS2812B บนแถบ LED */

/* =========================================================================
 * โครงสร้าง Flag – bit-field สำหรับเก็บ event / สถานะต่างๆ
 *   ใช้ volatile เพราะค่าเหล่านี้ถูกเขียนโดย ISR และอ่านจาก main loop
 *   ใช้ bit-field (: 1) ประหยัด RAM – แต่ละ flag ใช้เพียง 1 bit
 * ========================================================================= */
typedef struct {

    /* ---- flag จาก Timer2 ISR (tick ทุก 1 ms) ---- */
    unsigned mscF        : 1;  /* msc นับถอยหลังถึงศูนย์แล้ว          */
    unsigned msc2F       : 1;  /* msc2 นับถอยหลังถึงศูนย์แล้ว         */
    unsigned msc3F       : 1;
    unsigned msc4F       : 1;
    unsigned msc5F       : 1;
    unsigned msc6F       : 1;
    unsigned msc7F       : 1;
    unsigned msc8F       : 1;
    unsigned mscTOF      : 1;  /* mscTO timeout ครบแล้ว                */
    unsigned mscTO2F     : 1;  /* mscTO2 timeout ครบแล้ว               */

    /* ---- flag นับเวลาระดับ second ---- */
    unsigned msecF       : 1;  /* msec นับถอยหลังถึงศูนย์แล้ว         */
    unsigned msec2F      : 1;
    unsigned msecTOF     : 1;  /* msecTO timeout ครบแล้ว               */
    unsigned msecTO2F    : 1;  /* msecTO2 timeout ครบแล้ว (PN532 reinit) */
    unsigned msecTO3F    : 1;  /* msecTO3 timeout ครบแล้ว (auth timeout) */
    unsigned msecTO4F    : 1;  /* msecTO4 timeout ครบแล้ว (charge timeout) */
    unsigned msecTO5F    : 1;  /* msecTO5 timeout ครบแล้ว (connect timeout) */
    unsigned msecCalTOF  : 1;
    unsigned msecSyncF   : 1;

    /* ---- สถานะทั่วไป ---- */
    unsigned OKF         : 1;  /* ดำเนินการสำเร็จ             */
    unsigned ErrF        : 1;  /* เกิดข้อผิดพลาด              */

    /* ---- flag เกี่ยวกับ RTC / ข้อมูล ---- */
    unsigned RdStatusF   : 1;  /* ต้องการอ่านสถานะ             */
    unsigned RdDateF     : 1;  /* ต้องการอ่านวันที่             */
    unsigned DeviceIDF   : 1;  /* ต้องการอ่าน Device ID        */
    unsigned StatusF     : 1;

    /* ---- event จากปุ่มบน Nextion (หน้าหลัก) ---- */
    unsigned b0F : 1;  /* กดปุ่ม b0 "บันทึก RFID"              */
    unsigned b1F : 1;  /* กดปุ่ม b1 "ลบ RFID ทั้งหมด"         */
    unsigned b2F : 1;  /* กดปุ่ม b2 "ลด"                       */
    unsigned b3F : 1;  /* กดปุ่ม b3 "เพิ่ม"                    */
    unsigned b5F : 1;  /* กดปุ่ม b5 (เวลาชาร์จ 1 ชั่วโมง)     */
    unsigned b6F : 1;  /* กดปุ่ม b6 (เวลาชาร์จ 2 ชั่วโมง)     */
    unsigned b7F : 1;  /* กดปุ่ม b7 (เวลาชาร์จ 3 ชั่วโมง)     */
    unsigned b8F : 1;  /* กดปุ่ม b8 (ชาร์จไม่จำกัดเวลา)       */

    /* ---- event จาก text field บน Nextion ---- */
    unsigned t0F : 1;  /* แตะ t0 (แถว RFID slot 0)             */
    unsigned t1F : 1;  /* แตะ t1 (แถว RFID slot 1)             */
    unsigned t2F : 1;  /* แตะ t2 (แถว RFID slot 2)             */
    unsigned t3F : 1;  /* แตะ t3 (แถว RFID slot 3)             */
    unsigned t4F : 1;  /* แตะ t4 (แถว RFID slot 4)             */
    unsigned t5F : 1;  /* แตะ t5 (แถว RFID slot 5)             */

    /* ---- event จาก message widget บน Nextion ---- */
    unsigned m0F : 1;  /* message m0 ถูก tap                    */
    unsigned m1F : 1;  /* message m1 ถูก tap                    */
    unsigned m2F : 1;  /* message m2 ถูก tap                    */
    unsigned m3F : 1;  /* message m3 ถูก tap                    */
    unsigned m4F : 1;  /* message m4 ถูก tap                    */
    unsigned m5F : 1;

    /* ---- flag ระบบและการสื่อสาร ---- */
    unsigned ChkIDF      : 1;
    unsigned IDF         : 1;
    unsigned RdHumd0F    : 1;  /* ต้องการอ่านความชื้นช่อง 0           */
    unsigned RdHumd1F    : 1;  /* ต้องการอ่านความชื้นช่อง 1           */
    unsigned RdBattVF    : 1;  /* ต้องการอ่านแรงดันแบตเตอรี่          */
    unsigned DatRcvF     : 1;  /* รับข้อมูลใหม่แล้ว                   */
    unsigned RTCUdtF     : 1;  /* ต้องอัปเดต RTC                      */
    unsigned PlotF       : 1;  /* ต้องการพล็อตกราฟ                    */
    unsigned RdHMonF     : 1;  /* ต้องการอ่านชั่วโมงเปิด              */
    unsigned RdHMoffF    : 1;  /* ต้องการอ่านชั่วโมงปิด               */
    unsigned RdTonF      : 1;  /* ต้องการอ่านเวลาเปิด                 */
    unsigned RdToffF     : 1;  /* ต้องการอ่านเวลาปิด                  */
    unsigned getF        : 1;  /* รับค่าตัวเลขจาก Nextion แล้ว        */
    unsigned PgUdtF      : 1;  /* หน้า Nextion เปลี่ยนแล้ว            */
    unsigned PgUdtintF   : 1;  /* การเปลี่ยนหน้า Nextion (interrupt)  */

    /* ---- flag สำหรับ RFID / PN532 ---- */
    unsigned TagDetF     : 1;  /* ตรวจพบบัตรอยู่ในระยะ (ยังอ่านไม่ครบ) */
    unsigned TagFoundF   : 1;  /* เจอ header ของ tag แล้ว กำลังรับ UID  */
    unsigned PN532RdyF   : 1;  /* PN532 ตอบ ACK พร้อมรับคำสั่งแล้ว      */
    unsigned PN532GotF   : 1;  /* ได้ UID ครบ 10 byte แล้ว พร้อมประมวลผล */

    /* ---- flag การยืนยันตัวตน (Authentication) ---- */
    unsigned AuthF       : 1;  /* 1 = บัตร admin ผ่านแล้ว (โหมดตั้งค่า) */
    unsigned Auth2F      : 1;  /* 1 = บัตรผู้ใช้ผ่านแล้ว (เริ่ม/หยุดชาร์จ) */
    unsigned Auth3F      : 1;  /* สำรอง                                    */

    /* ---- flag ควบคุม relay และ CP ---- */
    unsigned CPenb       : 1;  /* 1 = เปิด CP PWM (ชาร์จ), 0 = CP = +12V (หยุด) */
    unsigned relay1Prv   : 1;  /* สถานะ relay ก่อนหน้า (ใช้ตรวจจับการเปลี่ยนแปลง) */

} Flag_t;

/* =========================================================================
 * การประกาศตัวแปร Global (extern)
 *   ตัวแปรจริงถูกกำหนดใน ev_charger.c
 *   ทุกตัวแปรที่ ISR เขียนหรืออ่านต้องประกาศเป็น volatile
 * ========================================================================= */

/* บัฟเฟอร์รับข้อมูล UART */
extern volatile Flag_t Flag;
extern volatile uint8_t  rxBufNextion[BUF_NEXTION_LEN]; /* รับจาก Nextion (UART1) */
extern volatile uint8_t  rxBufRfid[BUF_RFID_LEN];       /* รับจาก PN532 (UART2)   */
extern volatile uint8_t  rxBufLora[BUF_LORA_LEN];       /* รับจาก LoRa (UART3)    */
extern volatile uint8_t  rxBufAux[BUF_AUX_LEN];         /* รับจาก UART4 (สำรอง)  */

/* ตัวชี้ตำแหน่งใน buffer รับ UART */
extern volatile uint16_t rxIdxNextion;     /* ตำแหน่งปัจจุบันใน rxBufNextion    */
extern volatile uint16_t rxIdxNextionPrv;  /* ตำแหน่งก่อนหน้า (ตรวจ inactivity) */
extern volatile uint16_t rxIdxNextion2;    /* ตัวนับเสริม                         */
extern volatile uint16_t rxIdxRfid;        /* ตำแหน่งปัจจุบันใน rxBufRfid        */
extern volatile uint16_t rxIdxRfidPrv;

/* ADC / CP pilot signal */
extern volatile uint16_t cpAdcRaw;   /* ค่า ADC ล่าสุดจาก interrupt (0–1023) */
extern volatile uint16_t cpAdcAvg;   /* ค่าเฉลี่ยแบบ running average          */
extern volatile uint16_t cpAdcPrev;  /* ค่า ADC ที่เสถียรล่าสุด               */

/* State machine */
extern volatile JobStep_t jobStep;       /* สถานะปัจจุบันของกระบวนการชาร์จ */
extern volatile CpState_t chargingState; /* สถานะที่ decode จากสัญญาณ CP    */

/* CP PWM: OC1 สร้าง waveform จริง, Timer3 trigger ให้ ADC อ่านช่วง High */
extern volatile uint8_t pwmDuty;     /* duty cycle ปัจจุบัน (40–60)               */
extern volatile uint8_t pwmDutyPrev; /* duty cycle ก่อนหน้า (ตรวจการเปลี่ยนแปลง) */

/* LED WS2812B */
extern volatile LedRgb_t ledArray[LED_COUNT]; /* ข้อมูลสีของ LED แต่ละดวง          */

/* ตัวจับเวลา (นับถอยหลัง หน่วย ms ลดลงทุก tick โดย T2 ISR) */
extern volatile uint16_t msc;     /* ใช้โดย delayMs() – รอจนถึง 0 แล้วตั้ง Flag.mscF  */
extern volatile uint16_t msc2;    /* timer เอนกประสงค์ 2                               */
extern volatile uint16_t mscTO;   /* timeout สำหรับ RFID poll                          */
extern volatile uint16_t mscTO2;  /* timeout 2                                         */
extern volatile uint16_t msecTO;  /* timeout หลักใน main loop                          */
extern volatile uint16_t msecTO2; /* timeout สำหรับ PN532 reinit (60 วินาที)           */
extern volatile uint16_t msecTO3; /* timeout สำหรับ auth mode (10 วินาที)              */
extern volatile uint16_t msecTO4; /* timeout รอสัญญาณชาร์จ (5 วินาที)                 */
extern volatile uint16_t msecTO5; /* timeout รอรถพร้อม (30 วินาที)                     */
extern volatile uint16_t msecNextionPoll;  /* countdown poll ค่า PWM จาก Nextion          */
extern volatile uint16_t msecCpRawUpdate;  /* countdown update ค่า ADC debug บน Nextion   */
extern volatile uint16_t msecRfidReadDelay;/* countdown ก่อนส่ง PN532 read command ถัดไป */
extern volatile uint16_t msecPn532InitDelay;/* countdown state machine init PN532          */
extern volatile uint16_t msecAuthBeepDelay; /* countdown pattern เสียง auth แบบไม่ block   */
extern volatile uint16_t msecDim; /* countdown สำหรับ dim หน้าจอ Nextion               */
extern volatile uint16_t msecDim2;/* countdown สำหรับ dim LED ภายนอก                   */
extern volatile uint16_t msecUp2; /* ตัวนับเวลาทั่วไป                                 */

/* RFID */
extern volatile uint8_t  pn532Buf[PN532_TAG_BYTES]; /* raw bytes จาก PN532 หลังเจอ tag */
extern volatile uint8_t  pn532Count;    /* จำนวน byte ที่รับมาแล้วใน pn532Buf         */
extern volatile uint32_t rfidTagId;     /* UID ของบัตรที่สแกนล่าสุด (4 byte → uint32) */
extern volatile uint32_t rfidAuth1;     /* UID บัตรที่บันทึกไว้ slot 1                */
extern volatile uint32_t rfidAuth2;     /* UID บัตรที่บันทึกไว้ slot 2                */
extern volatile uint32_t rfidAuth3;     /* UID บัตรที่บันทึกไว้ slot 3                */
extern volatile uint32_t rfidAuth4;     /* UID บัตรที่บันทึกไว้ slot 4                */
extern volatile uint32_t rfidAuth5;     /* UID บัตรที่บันทึกไว้ slot 5                */
extern volatile uint32_t rfidAuthAdmin; /* UID ของบัตรที่เปิดโหมด admin ล่าสุด        */

/* ตัวแปรอื่นๆ */
extern volatile uint32_t relayCnt;   /* จำนวนครั้งสะสมที่ relay เคยทำงาน (ตลอดชีวิต) */
extern volatile uint16_t hexValue;   /* ค่า hex ที่รับมาจาก Nextion                   */
extern volatile uint32_t nxtValue;   /* ค่าตัวเลข 32 bit จาก Nextion slider/value     */
extern volatile uint8_t  nexPage;    /* หน้า Nextion ปัจจุบัน                          */
extern volatile uint8_t  nexPagePrev;/* หน้า Nextion ก่อนหน้า                          */

/* บัซเซอร์ */
extern volatile uint8_t  buzzTempo;    /* รูปแบบเสียง: 0=เงียบ, 1=ดังต่อเนื่อง, 10=หยุด, 11-19=กี่ครั้ง */
extern volatile uint8_t  buzzTempoPrev;/* ค่า buzzTempo ก่อนหน้า (ตรวจการเปลี่ยนแปลง) */
extern volatile uint16_t buzzMs;       /* ความยาวของแต่ละ beep (ms)                   */
extern volatile uint16_t buzzMs2;      /* countdown ระหว่าง beep                      */

/* นาฬิกาเวลา (RTC) */
extern volatile uint8_t rtcHour; /* ชั่วโมงปัจจุบัน (0–23) */
extern volatile uint8_t rtcMin;  /* นาทีปัจจุบัน (0–59)    */
extern volatile uint8_t rtcSec;  /* วินาทีปัจจุบัน (0–59)  */

/* บัฟเฟอร์ชั่วคราว (ห้ามเรียกจาก ISR!) */
extern char dispBuf[BUF_DISPLAY_LEN];  /* สำหรับสร้างข้อความแสดงผล          */
extern char atoiBuf[BUF_ATOI_LEN];     /* สำหรับแปลงตัวเลขเป็น string (หลัก) */
extern char atoiBuf2[BUF_ATOI_LEN];    /* สำรอง                              */
extern char atoiBuf3[BUF_ATOI_LEN];    /* สำรอง                              */

/* =========================================================================
 * ข้อมูล ROM (คงที่, เก็บใน Flash)
 * ========================================================================= */
extern const uint8_t pn532InitCmd[PN532_INIT_LEN]; /* command เริ่มต้น PN532 */
extern const uint8_t pn532ReadCmd[PN532_READ_LEN];  /* command อ่านบัตร PN532 */
extern const uint8_t qrPayload[];                   /* payload QR PromptPay   */

/* =========================================================================
 * ประกาศ Function (Prototypes)
 * ========================================================================= */

/* ---- delay ---- */
void     delayMs(uint16_t ms);   /* รอ (บล็อก) หน่วย ms โดยใช้ T2 ISR  */
void     delayUs(uint32_t us);   /* busy-wait แบบ legacy/โดยประมาณ      */
void     evChargerServiceRx(void); /* parse UART RX queue นอก ISR        */
void     evChargerFlushNextionRx(void); /* ทิ้งข้อมูลค้างใน UART1 RX queue/FIFO */
void     evChargerFlushRfidRx(void);    /* ทิ้งข้อมูลค้างใน UART2 RX queue/FIFO */

/* ---- I2C / EEPROM ---- */
void     i2cClock(void);                               /* สร้าง pulse clock 1 ครั้ง      */
uint8_t  eepromReadByte(uint16_t addr);                /* อ่าน 1 byte จาก EEPROM         */
uint16_t eepromReadWord(uint16_t addr);                /* อ่าน 2 byte จาก EEPROM         */
uint32_t eepromReadLong(uint16_t addr);                /* อ่าน 4 byte จาก EEPROM         */
void     eepromWriteByte(uint16_t addr, uint8_t  val); /* เขียน 1 byte ลง EEPROM         */
void     eepromWriteWord(uint16_t addr, uint16_t val); /* เขียน 2 byte ลง EEPROM         */
void     eepromWriteLong(uint16_t addr, uint32_t val); /* เขียน 4 byte ลง EEPROM         */

/* ---- UART ---- */
void uartPutChar1(uint8_t c);       /* ส่ง 1 byte ทาง UART1 (Nextion)     */
void uartPutStr1(const char *s);    /* ส่ง string ทาง UART1               */
void uartPutChar2(uint8_t c);       /* ส่ง 1 byte ทาง UART2 (PN532)       */
void uartPutStr2(const char *s);    /* ส่ง string ทาง UART2               */
void uartPutChar3(char c);          /* ส่ง 1 byte ทาง UART3 (LoRa)        */
void uartPutStr3(const char *s);    /* ส่ง string ทาง UART3               */
void uartPutChar4(char c);          /* ส่ง 1 byte ทาง UART4               */
void uartPutStr4(const char *s);    /* ส่ง string ทาง UART4               */

/* ---- Nextion display ---- */
void nxtSendTerminator(void);                            /* ส่ง 0xFF×3 ปิดท้าย command    */
void nxtSendText(const char *widget, const char *value); /* กำหนดค่า widget               */
void nxtDefaultLayout(void);                             /* ซ่อนปุ่มและโหลด preset        */
void nxtHighlightSelected(uint8_t slot);                 /* ไฮไลต์ slot RFID ที่เลือก     */
void nxtShowVersion(void);                               /* แสดงเวอร์ชัน firmware บนจอ    */
void nxtSetBrightness(uint8_t pct);                      /* ตั้งความสว่างหน้าจอ (0–100)   */

/* ---- LED WS2812B ---- */
void ws2812SendByte(uint8_t val);              /* ส่ง 1 byte ผ่าน bit-bang         */
void ws2812Update(void);                       /* ส่งข้อมูล ledArray[] แบบ blocking */
void ws2812SetAll(uint8_t g, uint8_t r, uint8_t b); /* ตั้งสี LED ทุกดวงพร้อมกัน  */

/* ---- RFID / PN532 ---- */
void     pn532Init(void);                    /* ส่ง init sequence และรอ PN532 พร้อม    */
void     pn532InitAsyncStart(void);           /* เริ่ม init PN532 แบบไม่ block          */
void     pn532InitAsyncService(void);         /* เดิน state machine init PN532          */
bool     pn532InitAsyncBusy(void);            /* true ถ้า init PN532 ยังไม่เสร็จ        */
void     pn532RequestRead(void);             /* สั่ง PN532 อ่านบัตรครั้งถัดไป          */
uint32_t pn532ParseTagId(void);              /* แปลง pn532Buf → UID uint32_t           */
bool     rfidIsAuthorised(uint32_t tagId);   /* ตรวจว่า UID นี้มีสิทธิ์ชาร์จหรือไม่   */
void     rfidRefreshDisplay(void);           /* โหลด slot จาก EEPROM แสดงบน Nextion   */
void     rfidRecordTag(uint32_t tagId);      /* บันทึก UID ใหม่ (เลื่อน slot เก่าลง)  */
void     rfidEraseAll(void);                 /* ลบ UID ทั้ง 5 slot                     */

/* ---- CP pilot ---- */
CpState_t cpDecodePilot(uint16_t adcVal); /* แปลงค่า ADC เป็น CpState_t         */
void      cpShowStatus(CpState_t state);  /* แสดงสถานะ CP บน Nextion widget msg0 */

/* ---- State machine ---- */
void stateMachineRun(CpState_t cpState); /* รัน 1 รอบของ JobStep state machine */

/* ---- CRC (สำหรับ QR PromptPay) ---- */
uint16_t crc16CcittFalse(const uint8_t *data, uint16_t len); /* CRC-16/CCITT-FALSE */

/* ---- ISR (กำหนดใน ev_charger_isr_main.c) ---- */
void __attribute__((interrupt, no_auto_psv)) _T2Interrupt(void);  /* 1 ms   – timers      */
void __attribute__((interrupt, auto_psv))    _ISR _U1RXInterrupt(void); /* Nextion RX      */
void __attribute__((interrupt, auto_psv))    _ISR _U2RXInterrupt(void); /* PN532 RX        */
void __attribute__((interrupt, auto_psv))    _ISR _ADC1Interrupt(void); /* ADC CP signal   */

#endif /* EV_CHARGER_H */
