/**
 * @file    ev_charger.c
 * @brief   Firmware เครื่องชาร์จรถยนต์ไฟฟ้า – ส่วนฟังก์ชันหลัก
 * @version v2.04r  (ปรับปรุงจาก v2.04a)
 *
 * ไฟล์นี้ประกอบด้วย:
 *   1. ค่าคงที่ ROM (command PN532, QR payload, รายการบัตร admin)
 *   2. ตัวแปร global ทั้งหมด
 *   3. ฟังก์ชัน delay
 *   4. ไดรเวอร์ I2C (bit-bang) และ EEPROM
 *   5. ฟังก์ชันส่งข้อมูล UART 1–4
 *   6. ฟังก์ชันควบคุมหน้าจอ Nextion
 *   7. ไดรเวอร์ LED WS2812B
 *   8. ฟังก์ชันสำหรับ RFID / PN532
 *   9. ฟังก์ชัน decode สัญญาณ CP และแสดงสถานะ
 *  10. State machine ของกระบวนการชาร์จ
 *  11. ฟังก์ชัน CRC-16 สำหรับ QR PromptPay
 */

/* =========================================================================
 * การตั้งค่า Fuse ของ MCU (Configuration Bits)
 *   Fuse เหล่านี้ถูก burn ลง flash ตอน program และส่งผลตั้งแต่เปิดเครื่อง
 *   ก่อน main() ทำงาน
 * ========================================================================= */
#ifndef MCU_VARIANT
#define MCU_VARIANT  1
#endif

/* XC16 v2.10 เตือน deprecated เมื่อใช้ _CONFIGx() macro เดิม
 * แต่ #pragma config ของ device นี้เปลี่ยน reserved/default bits จาก v2.04a
 * จึงวาง config word แบบ explicit เพื่อให้ fuse word ใน HEX คงเดิมทุกบิต */
#if MCU_VARIANT == 1
  #include <p24FJ128GA106.h>
  /* CONFIG1:
   *   JTAGEN_OFF  = ปิด JTAG (ป้องกันการอ่าน flash โดยไม่ได้รับอนุญาต)
   *   GCP_ON      = เปิด General Code Protection (ป้องกัน flash)
   *   FWDTEN_OFF  = ปิด Watchdog Timer (เปิดใช้ WDT ใน software แทน)
   *   FWPSA_PR32  = Watchdog prescaler 1:32
   *   WDTPS_PS1024= Watchdog postscaler 1:1024 (≈ 1 วินาที) */
  #define CONFIG1_WORD_GA106  (JTAGEN_OFF & GCP_ON & FWDTEN_OFF & FWPSA_PR32 & WDTPS_PS1024)
  __attribute__((address(0x157FE), section("__CONFIG1.sec"), space(prog)))
  __prog__ unsigned int config1_word_ga106 = CONFIG1_WORD_GA106;

  /* CONFIG2:
   *   FNOSC_FRCPLL = ใช้ internal RC oscillator × PLL → Fosc ประมาณ 32 MHz
   *                  ดังนั้น instruction cycle Fcy ประมาณ 16 MHz
   *   POSCMOD_NONE = ไม่ใช้ external oscillator
   *   FCKSM_CSDCMD = ปิดการสลับ clock และ clock monitor
   *   OSCIOFNC_ON  = ขา OSC2 ใช้เป็น GPIO ได้ */
  #define CONFIG2_WORD_GA106  (FNOSC_FRCPLL & POSCMOD_NONE & FCKSM_CSDCMD & OSCIOFNC_ON)
  __attribute__((address(0x157FC), section("__CONFIG2.sec"), space(prog)))
  __prog__ unsigned int config2_word_ga106 = CONFIG2_WORD_GA106;
#else
  #include <p24FJ128GB106.h>
  #define CONFIG2_WORD_GB106  (IESO_OFF & PLL_96MHZ_ON & PLLDIV_DIV2 & FNOSC_FRCPLL & POSCMOD_NONE)
  __attribute__((address(0x157FC), section("__CONFIG2.sec"), space(prog)))
  __prog__ unsigned int config2_word_gb106 = CONFIG2_WORD_GB106;

  #define CONFIG1_WORD_GB106  (JTAGEN_OFF & ICS_PGx2 & FWDTEN_OFF)
  __attribute__((address(0x157FE), section("__CONFIG1.sec"), space(prog)))
  __prog__ unsigned int config1_word_gb106 = CONFIG1_WORD_GB106;
#endif

#include "ev_charger.h"

/* =========================================================================
 * ค่าคงที่ใน ROM (Flash Program Memory)
 * ========================================================================= */

/**
 * คำสั่งปลุก PN532 จาก power-down mode และตั้งค่า SAM configuration
 * (28 byte: preamble + wake-up bytes + SAMConfiguration command)
 * ส่งทาง UART2 ด้วย baud rate 115200
 */
const uint8_t pn532InitCmd[PN532_INIT_LEN] = {
    0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xFF,0x03,0xFD,0xD4,0x14,0x01,0x17,0x00
};

/**
 * คำสั่ง InListPassiveTarget – สั่ง PN532 สแกนหาบัตร ISO14443A ใน mode passive
 * (11 byte: TFI + command code + parameters)
 * ส่งทาง UART2 ทุกครั้งที่ต้องการอ่านบัตรรอบใหม่
 */
const uint8_t pn532ReadCmd[PN532_READ_LEN] = {
    0x00,0x00,0xFF,0x04,0xFC,0xD4,0x4A,0x02,0x00,0xE0,0x00
};

/**
 * Payload สำหรับสร้าง QR Code PromptPay
 * รูปแบบตามมาตรฐาน EMVCo QR Code Specification สำหรับ Thailand
 * (ยังไม่ได้ต่อ CRC-16 ท้าย – คำนวณ runtime ด้วย crc16CcittFalse())
 */
const uint8_t qrPayload[] =
    "00020101021129370016A000000677010111011300668191159585802TH5405";

/**
 * รายการ UID บัตร RFID ของ admin (เจ้าหน้าที่ / ผู้ดูแลระบบ)
 * บัตร admin ใดบัตรหนึ่งในรายการนี้สามารถเปิดโหมดตั้งค่าพารามิเตอร์ได้
 * ข้อสำคัญ: ค่าทุกตัวต้องอยู่ใน uint32_t (≤ 4,294,967,295)
 */
static const uint32_t tagAdminList[] = {
    TAG_ADMIN_0,  /* บัตร admin 1 */
    TAG_ADMIN_1,  /* บัตร admin 2 */
    TAG_ADMIN_2,  /* บัตร admin 3 */
    TAG_ADMIN_3   /* บัตร admin 4 (แก้ค่าจากเดิมที่เกิน 32 bit) */
};
/* จำนวนบัตร admin ทั้งหมด (คำนวณอัตโนมัติจากขนาด array) */
#define TAG_ADMIN_COUNT  (sizeof(tagAdminList) / sizeof(tagAdminList[0]))

/* =========================================================================
 * ตัวแปร Global – ประกาศจริงที่นี่ (extern ประกาศใน header)
 *   volatile: บอก compiler ว่าค่าอาจเปลี่ยนได้ทุกเมื่อ (โดย ISR)
 *             ห้าม compiler optimize ออก หรือ cache ไว้ใน register
 * ========================================================================= */

/* บัฟเฟอร์รับข้อมูล UART (เขียนโดย ISR, อ่านโดย main loop) */
volatile Flag_t Flag;
volatile uint8_t  rxBufNextion[BUF_NEXTION_LEN]; /* รับจาก Nextion display (UART1) */
volatile uint8_t  rxBufRfid[BUF_RFID_LEN];       /* รับจาก PN532 RFID reader (UART2) */
volatile uint8_t  rxBufLora[BUF_LORA_LEN];       /* รับจาก LoRa module (UART3)       */
volatile uint8_t  rxBufAux[BUF_AUX_LEN];         /* รับจาก UART4 (สำรอง)            */

/* ตัวชี้ตำแหน่งใน buffer UART */
volatile uint16_t rxIdxNextion    = 0;  /* ตำแหน่งถัดไปที่จะเขียนใน rxBufNextion    */
volatile uint16_t rxIdxNextionPrv = 0;  /* ค่าก่อนหน้า ใช้ตรวจ inactivity ของหน้าจอ */
volatile uint16_t rxIdxNextion2   = 0;  /* ตัวนับเสริม                               */
volatile uint16_t rxIdxRfid       = 0;  /* ตำแหน่งถัดไปที่จะเขียนใน rxBufRfid        */
volatile uint16_t rxIdxRfidPrv    = 0;

/* สัญญาณ CP (Control Pilot) – ค่าจาก ADC */
volatile uint16_t cpAdcRaw  = 0;  /* ค่า ADC ดิบล่าสุดจาก interrupt (0–1023)         */
volatile uint16_t cpAdcAvg  = 0;  /* ค่าเฉลี่ยสะสม (running average ÷ 2 ทุก sample) */
volatile uint16_t cpAdcPrev = 0;  /* ค่าเฉลี่ยที่เสถียรล่าสุด (อ่านโดย T1 ISR)      */

/* State machine */
volatile JobStep_t jobStep      = JOB_IDLE;   /* สถานะการชาร์จปัจจุบัน    */
volatile CpState_t chargingState = CP_FLOAT;  /* สถานะ CP ปัจจุบัน         */

/* CP PWM: OC1 สร้าง waveform จริง, Timer3 trigger ให้ ADC อ่านช่วง High */
volatile uint8_t pwmDuty     = PWM_DUTY_DEFAULT; /* duty cycle ปัจจุบัน      */
volatile uint8_t pwmDutyPrev = PWM_DUTY_DEFAULT; /* duty cycle ก่อนหน้า      */

/* LED WS2812B */
volatile LedRgb_t ledArray[LED_COUNT]; /* ข้อมูลสีของ LED แต่ละดวง (G,R,B)        */

/* ตัวจับเวลาแบบ countdown (T2 ISR ลดค่าทุก 1 ms) */
volatile uint16_t msc    = 0;  /* ใช้โดย delayMs() – Flag.mscF ถูก set เมื่อนับถึง 0     */
volatile uint16_t msc2   = 0;  /* ตัวจับเวลาเอนกประสงค์ 2                                */
volatile uint16_t msc3   = 0;  /* ตัวนับขึ้น (ไม่ลดลง ใช้วัด elapsed time)              */
volatile uint16_t msc4   = 0;
volatile uint16_t msc5   = 0;
volatile uint16_t msc6   = 0;
volatile uint16_t mscTO  = 0;  /* timeout สำหรับ RFID poll (ตรวจบัตรผิดปกติ)             */
volatile uint16_t mscTO2 = 0;
volatile uint16_t msecTO  = 0; /* timeout หลักใน main loop (1 วินาที default)             */
volatile uint16_t msecTO2 = 0; /* timeout สำหรับ PN532 reinit ทุก 60 วินาที              */
volatile uint16_t msecTO3 = 0; /* timeout admin mode (10 วินาทีหลังสแกนบัตร admin)       */
volatile uint16_t msecTO4 = 0; /* timeout รอสัญญาณชาร์จ (5 วินาที)                       */
volatile uint16_t msecTO5 = 0; /* timeout รอรถพร้อมชาร์จ (30 วินาที)                     */
volatile uint16_t msecNextionPoll = 0;  /* รอบถามค่า PWM จาก Nextion แบบไม่ block main loop */
volatile uint16_t msecCpRawUpdate = 0;  /* รอบ update ค่า ADC debug บน Nextion              */
volatile uint16_t msecRfidReadDelay = 0; /* รอก่อนสั่ง PN532 อ่านบัตรรอบถัดไปแบบไม่ block */
volatile uint16_t msecPn532InitDelay = 0; /* timer สำหรับ PN532 init state machine */
volatile uint16_t msecAuthBeepDelay = 0; /* timer สำหรับเสียง auth pattern แบบไม่ block */
volatile uint16_t msecDim  = 0; /* countdown ก่อน dim หน้าจอ Nextion (60 วินาที default) */
volatile uint16_t msecDim2 = 0; /* countdown ก่อน dim LED ภายนอก                         */
volatile uint16_t msecUp2  = 0; /* ตัวนับเวลาทั่วไป                                       */
volatile uint16_t buzzMs   = 0; /* ความยาวของแต่ละ beep (ms)                              */
volatile uint16_t buzzMs2  = 0; /* countdown ระยะเวลาระหว่าง beep                         */

/* RFID */
volatile uint8_t  pn532Buf[PN532_TAG_BYTES]; /* raw bytes จาก PN532 หลังเจอ tag header */
volatile uint8_t  pn532Count    = 0;  /* จำนวน byte ที่รับมาแล้วใน pn532Buf             */
volatile uint32_t rfidTagId     = 0;  /* UID บัตรปัจจุบัน (0 = ยังไม่มีบัตร)            */
volatile uint32_t rfidAuth1     = 0;  /* UID slot 1 (โหลดจาก EEPROM ตอน startup)        */
volatile uint32_t rfidAuth2     = 0;  /* UID slot 2                                     */
volatile uint32_t rfidAuth3     = 0;  /* UID slot 3                                     */
volatile uint32_t rfidAuth4     = 0;  /* UID slot 4                                     */
volatile uint32_t rfidAuth5     = 0;  /* UID slot 5 (เก่าที่สุด)                        */
volatile uint32_t rfidAuthAdmin = 0;  /* UID ของบัตรที่เปิด admin mode ล่าสุด           */

/* ตัวนับ relay */
volatile uint32_t relayCnt = 0; /* จำนวนครั้ง relay เคยต่อวงจร (บันทึกลง EEPROM)      */

/* Nextion display */
volatile uint16_t hexValue    = 0; /* ค่า hex ที่รับมาจาก Nextion ผ่าน 'q' command      */
volatile uint32_t nxtValue    = 0; /* ค่าตัวเลขจาก Nextion slider / numeric widget       */
volatile uint8_t  nexPage     = 0; /* หน้า Nextion ที่แสดงอยู่ปัจจุบัน                  */
volatile uint8_t  nexPagePrev = 0; /* หน้า Nextion ก่อนหน้า (ตรวจการเปลี่ยนหน้า)       */

/* บัซเซอร์ */
volatile uint8_t buzzTempo     = 0; /* รูปแบบเสียง: 0=เงียบ 1=ดัง 10=หยุด 11-19=N ครั้ง */
volatile uint8_t buzzTempoPrev = 0; /* ค่าก่อนหน้า (ตรวจการเปลี่ยนแปลง)                */

/* นาฬิกาเวลา */
volatile uint8_t rtcHour = 0; /* ชั่วโมง (0–23) */
volatile uint8_t rtcMin  = 0; /* นาที (0–59)    */
volatile uint8_t rtcSec  = 0; /* วินาที (0–59)  */

/* บัฟเฟอร์ชั่วคราว – ใช้เฉพาะใน main loop / function ปกติ (ห้ามเรียกจาก ISR) */
char dispBuf[BUF_DISPLAY_LEN]; /* สำหรับสร้างข้อความแสดงผลก่อนส่งไป Nextion */
char atoiBuf[BUF_ATOI_LEN];    /* buffer แปลงตัวเลขเป็น string (sprintf)      */
char atoiBuf2[BUF_ATOI_LEN];   /* buffer แปลงตัวเลขเสริม 2                    */
char atoiBuf3[BUF_ATOI_LEN];   /* buffer แปลงตัวเลขเสริม 3                    */

/* =========================================================================
 * ฟังก์ชัน Delay
 * ========================================================================= */

/**
 * @brief  รอเวลา (blocking) โดยใช้ Timer2 ISR นับถอยหลัง
 *         ฟังก์ชันนี้จะวนรอจนกว่า Flag.mscF จะถูกตั้งโดย T2 ISR
 *         ห้ามเรียกจากภายใน ISR เพราะจะ deadlock
 * @param  ms  เวลาที่รอ (หน่วย ms, ค่าสูงสุด 65535)
 */
void delayMs(uint16_t ms)
{
    evChargerServiceRx();
    msc       = ms;    /* โหลดค่า countdown ลงตัวแปรที่ T2 ISR จะลดค่า */
    Flag.mscF = 0;     /* ล้าง flag เก่า                                  */
    while (!Flag.mscF)
    {
        evChargerServiceRx(); /* parse UART ระหว่างรอ เพื่อคงพฤติกรรมเดิม */
    }
}

/**
 * @brief  รอเวลาแบบ busy-wait สั้นๆ ตาม routine เดิมจาก v2.04a
 *         ค่า us เป็นหน่วยเชิง legacy/โดยประมาณ ไม่ใช่ microsecond
 *         ที่ calibrate จาก timer hardware
 * @param  us  จำนวนรอบ delay legacy
 */
void delayUs(uint32_t us)
{
    uint16_t y;
    while (us--)
    {
        /* Assembly ที่ build จริงมี 3 NOP ต่อรอบ บวก branch/loop overhead */
        for (y = 3u; y > 0u; y--) { asm("nop"); }
    }
}

/* =========================================================================
 * I2C แบบ bit-bang (Software I2C)
 *
 *   บอร์ดนี้ใช้ I2C แบบ bit-bang แทน hardware I2C
 *   เพราะขา SDA/SCL ถูกใช้ร่วมกับฟังก์ชันอื่น
 *
 *   ฟังก์ชัน primitive ภายใน (static) ไม่ควรเรียกจากภายนอกไฟล์นี้โดยตรง
 *   ใช้ผ่าน eepromReadXxx() / eepromWriteXxx() เท่านั้น
 * ========================================================================= */

#define EEPROM_I2C_ADDR_WRITE       0xA0u
#define EEPROM_I2C_ADDR_READ        0xA1u
#define EEPROM_WRITE_CYCLE_MAX_US   5000u
#define EEPROM_ACK_POLL_INTERVAL_US 100u

/**
 * @brief  สร้าง clock pulse 1 ครั้ง (H → L) สำหรับ I2C
 *         ใช้สำหรับทั้ง เขียน และ อ่าน bit ใน I2C
 */
void i2cClock(void)
{
    delayUs(5); I2C_SCL = 1; delayUs(5); I2C_SCL = 0; delayUs(3);
}

/**
 * @brief  ส่ง 1 byte บน I2C แล้วอ่าน ACK จริงจาก EEPROM
 * @return 1 = ได้ ACK, 0 = NACK
 */
static uint8_t i2cWriteByte(uint8_t data)
{
    uint8_t i;
    uint8_t ack;

    I2C_SDA_TRIS = 0;
    for (i = 0u; i < 8u; i++)
    {
        I2C_SDA_OUT = (data & 0x80u) ? 1 : 0;
        data <<= 1;
        i2cClock();
    }

    I2C_SDA_OUT = 1;
    I2C_SDA_TRIS = 1;       /* ปล่อย SDA ให้ EEPROM ดึงลงเมื่อ ACK */
    delayUs(5);
    I2C_SCL = 1;
    delayUs(5);
    ack = (I2C_SDA_IN == 0u) ? 1u : 0u;
    I2C_SCL = 0;
    delayUs(3);
    I2C_SDA_TRIS = 0;

    return ack;
}

/**
 * @brief  ส่ง START condition และ device address ไปยัง EEPROM
 *         EEPROM ของบอร์ดนี้มี address = 0xA0 (write) หรือ 0xA1 (read)
 * @param  rw  0 = write mode (device addr = 0xA0)
 *             1 = read mode  (device addr = 0xA1)
 * @return 1 = EEPROM ตอบ ACK, 0 = NACK
 */
static uint8_t i2cStart(uint8_t rw)
{
    uint8_t devAddr = rw ? EEPROM_I2C_ADDR_READ : EEPROM_I2C_ADDR_WRITE;

    I2C_SDA_TRIS = 0;           /* SDA เป็น output */
    I2C_SCL = 1;  delayUs(5);
    I2C_SDA_OUT = 1; delayUs(5);
    I2C_SDA_OUT = 0;            /* SDA ลงขณะ SCL สูง = START condition */
    delayUs(5); I2C_SCL = 0; delayUs(5);

    return i2cWriteByte(devAddr);
}

/**
 * @brief  ส่ง STOP condition เพื่อปิด transaction I2C
 *         STOP = SDA ขึ้นขณะ SCL สูง
 */
static void i2cStop(void)
{
    I2C_SDA_TRIS = 0;
    I2C_SDA_OUT = 0; delayUs(5);
    I2C_SCL = 1;     delayUs(5);
    I2C_SDA_OUT = 1;  /* STOP condition */
    delayUs(5);
    I2C_SCL = 0;
    delayUs(5);
}

/**
 * @brief  Reset bus โดยส่ง STOP condition เพื่อยกเลิก transaction ที่ค้างอยู่
 *         เรียกก่อนเริ่ม transaction ใหม่ทุกครั้ง เพื่อให้แน่ใจว่า bus ว่าง
 */
static void i2cBusReset(void)
{
    I2C_SDA_TRIS = 0;
    I2C_SCL = 0;     delayUs(5);
    I2C_SDA_OUT = 0; delayUs(5);
    I2C_SCL = 1;     delayUs(5);
    I2C_SDA_OUT = 1; /* STOP */
    delayUs(5);
}

/**
 * @brief  ส่ง 8 bit ของ address register ไปยัง EEPROM (MSB first)
 *         EEPROM ต้องการ address 16 bit จึงต้องเรียก 2 ครั้ง (High byte, Low byte)
 * @param  addrWord  16-bit address ที่จะส่ง (shift ไว้แล้ว)
 * @return 1 = EEPROM ตอบ ACK, 0 = NACK
 */
static uint8_t i2cSendAddrByte(uint16_t addrWord)
{
    return i2cWriteByte((uint8_t)(addrWord >> 8));
}

/**
 * @brief  รอ EEPROM จบ internal write cycle ด้วย ACK polling ตาม datasheet 24XX256
 * @return 1 = พร้อมก่อน timeout, 0 = ยัง NACK หลังครบเวลาสูงสุด
 */
static uint8_t eepromWaitReady(void)
{
    uint16_t waitedUs = 0u;
    uint8_t  ack;

    do
    {
        i2cBusReset();
        ack = i2cStart(0);
        i2cStop();
        if (ack)
        {
            return 1u;
        }
        delayUs(EEPROM_ACK_POLL_INTERVAL_US);
        waitedUs = (uint16_t)(waitedUs + EEPROM_ACK_POLL_INTERVAL_US);
    } while (waitedUs < EEPROM_WRITE_CYCLE_MAX_US);

    return 0u;
}

/**
 * @brief  อ่าน 1 byte จาก I2C bus
 * @param  sendAck  true  = ส่ง ACK (มีข้อมูลให้อ่านต่อ)
 *                  false = ส่ง NACK (byte สุดท้าย จะตามด้วย STOP)
 * @return byte ที่อ่านได้
 */
static uint8_t i2cReadByte(bool sendAck)
{
    uint8_t i;
    uint8_t val = 0u;

    I2C_SDA_OUT = 1; I2C_SDA_TRIS = 1; /* ปล่อย SDA ให้ EEPROM ขับ */

    /* อ่าน 7 bit แรก พร้อม shift ทุกครั้ง */
    for (i = 0u; i < 7u; i++)
    {
        I2C_SCL = 1; delayUs(5);
        if (I2C_SDA_IN) { val |= 0x01u; } /* sample SDA ขณะ SCL สูง */
        val <<= 1;                          /* เลื่อนซ้ายรอ bit ถัดไป */
        delayUs(5); I2C_SCL = 0; delayUs(5);
    }
    /* bit สุดท้าย – ไม่ shift ซ้ายอีก */
    I2C_SCL = 1; delayUs(5);
    if (I2C_SDA_IN) { val |= 0x01u; }
    delayUs(5); I2C_SCL = 0;

    /* ส่ง ACK หรือ NACK */
    I2C_SDA_TRIS = 0;
    I2C_SDA_OUT  = sendAck ? 0 : 1; /* ACK = 0, NACK = 1 */
    delayUs(5);
    I2C_SCL = 1; delayUs(5); I2C_SCL = 0;

    return val;
}

/* =========================================================================
 * EEPROM API สาธารณะ
 * ========================================================================= */

/**
 * @brief  อ่าน 1 byte จาก EEPROM ที่ address ที่กำหนด
 * @param  addr  ที่อยู่ EEPROM (0–65535)
 * @return ค่า byte ที่อ่านได้
 */
uint8_t eepromReadByte(uint16_t addr)
{
    uint8_t result;
    i2cBusReset();           /* รีเซ็ต bus ก่อนเริ่ม transaction ใหม่ */
    i2cStart(0);             /* write mode: กำหนด address ที่จะอ่าน  */
    i2cSendAddrByte(addr);   /* ส่ง address high byte                  */
    i2cSendAddrByte(addr << 8); /* ส่ง address low byte               */
    i2cStart(1);             /* repeated START แล้วสลับเป็น read mode  */
    result = i2cReadByte(false); /* อ่าน 1 byte แล้ว NACK (byte สุดท้าย) */
    i2cStop();
    return result;
}

/**
 * @brief  อ่าน 2 byte (unsigned int) จาก EEPROM (เก็บแบบ big-endian)
 * @param  addr  ที่อยู่ byte แรก
 * @return ค่า 16 bit
 */
uint16_t eepromReadWord(uint16_t addr)
{
    uint16_t result = 0u;
    i2cBusReset();
    i2cStart(0);
    i2cSendAddrByte(addr);
    i2cSendAddrByte(addr << 8);
    i2cStart(1);
    result  = (uint16_t)i2cReadByte(true) << 8; /* byte สูง */
    result |= i2cReadByte(false);                /* byte ต่ำ (NACK) */
    i2cStop();
    return result;
}

/**
 * @brief  อ่าน 4 byte (unsigned long) จาก EEPROM (เก็บแบบ big-endian)
 *         ใช้สำหรับอ่าน UID บัตร RFID และตัวนับ relay
 * @param  addr  ที่อยู่ byte แรก (MSB)
 * @return ค่า 32 bit
 */
uint32_t eepromReadLong(uint16_t addr)
{
    uint32_t result = 0u;
    uint8_t  i;
    i2cBusReset();
    i2cStart(0);
    i2cSendAddrByte(addr);
    i2cSendAddrByte(addr << 8);
    i2cStart(1);
    /* อ่าน byte 0–2 พร้อมส่ง ACK (ยังมีต่อ) */
    for (i = 0u; i < 3u; i++)
    {
        result |= (uint32_t)i2cReadByte(true);
        result <<= 8;
    }
    result |= (uint32_t)i2cReadByte(false); /* byte สุดท้าย + NACK */
    i2cStop();
    return result;
}

/**
 * @brief  เขียน 1 byte ลง EEPROM
 *         หลัง STOP จะรอ internal write cycle ด้วย ACK polling ตาม datasheet
 * @param  addr  ที่อยู่ EEPROM
 * @param  data  ค่า byte ที่ต้องการเขียน
 */
void eepromWriteByte(uint16_t addr, uint8_t data)
{
    i2cBusReset();
    i2cStart(0);             /* write mode */
    i2cSendAddrByte(addr);
    i2cSendAddrByte(addr << 8);
    i2cWriteByte(data);
    i2cStop();
    (void)eepromWaitReady();
}

/**
 * @brief  เขียน 2 byte (uint16_t) ลง EEPROM แบบ big-endian (MSB ก่อน)
 *         พอร์ตจาก EPWint() ใน firmware v2.04a
 * @param  addr  ที่อยู่ byte แรก
 * @param  data  ค่า 16 bit ที่ต้องการเขียน
 */
void eepromWriteWord(uint16_t addr, uint16_t data)
{
    uint8_t  j;
    uint16_t shifted = data;

    i2cBusReset();
    i2cStart(0);
    i2cSendAddrByte(addr);
    i2cSendAddrByte(addr << 8);

    for (j = 0u; j < 2u; j++)
    {
        i2cWriteByte((uint8_t)(shifted >> 8));
        shifted <<= 8;
    }
    i2cStop();
    (void)eepromWaitReady();
}

/**
 * @brief  เขียน 4 byte (uint32_t) ลง EEPROM แบบ big-endian (MSB ก่อน)
 *         ใช้สำหรับบันทึก UID บัตร RFID และตัวนับ relay
 * @param  addr  ที่อยู่ byte แรก
 * @param  data  ค่า 32 bit ที่ต้องการเขียน
 */
void eepromWriteLong(uint16_t addr, uint32_t data)
{
    uint8_t  j;
    uint32_t shifted = data;

    i2cBusReset();
    i2cStart(0);
    i2cSendAddrByte(addr);
    i2cSendAddrByte(addr << 8);

    /* เขียน 4 byte ทีละ byte (MSB ก่อน) */
    for (j = 0u; j < 4u; j++)
    {
        i2cWriteByte((uint8_t)(shifted >> 24));
        shifted <<= 8;
    }
    i2cStop();
    (void)eepromWaitReady();
}

/**
 * @brief  เขียน EEPROM เฉพาะเมื่อค่า byte เปลี่ยนจริง เพื่อลด write cycle
 */
static void eepromWriteByteIfChanged(uint16_t addr, uint8_t data)
{
    if (eepromReadByte(addr) != data)
    {
        eepromWriteByte(addr, data);
    }
}

/**
 * @brief  เขียน EEPROM เฉพาะเมื่อค่า long เปลี่ยนจริง เพื่อลด write cycle
 */
static void eepromWriteLongIfChanged(uint16_t addr, uint32_t data)
{
    if (eepromReadLong(addr) != data)
    {
        eepromWriteLong(addr, data);
    }
}

/* =========================================================================
 * ฟังก์ชันส่งข้อมูล UART
 *
 *   UART1: Nextion display (9600 baud)
 *   UART2: PN532 RFID reader (115200 baud)
 *   UART3: LoRa module
 *   UART4: สำรอง
 * ========================================================================= */

/** @brief  ส่ง 1 byte ทาง UART1 (Nextion) รอจนกว่า TX buffer จะว่าง */
void uartPutChar1(uint8_t c)
{
    while (U1STAbits.UTXBF) { /* รอ TX buffer ว่าง */ }
    U1TXREG = c;
}

/** @brief  ส่ง string (null-terminated) ทาง UART1 ทีละ byte */
void uartPutStr1(const char *s)
{
    while (*s) { uartPutChar1((uint8_t)*s++); }
}

/** @brief  ส่ง 1 byte ทาง UART2 (PN532 RFID) */
void uartPutChar2(uint8_t c)
{
    while (U2STAbits.UTXBF) { }
    U2TXREG = c;
}

void uartPutStr2(const char *s)
{
    while (*s) { uartPutChar2((uint8_t)*s++); }
}

void uartPutChar3(char c)
{
    while (U3STAbits.UTXBF) { }
    U3TXREG = c;
}

void uartPutStr3(const char *s)
{
    while (*s) { uartPutChar3(*s++); }
}

void uartPutChar4(char c)
{
    while (U4STAbits.UTXBF) { }
    U4TXREG = c;
}

void uartPutStr4(const char *s)
{
    while (*s) { uartPutChar4(*s++); }
}

/* =========================================================================
 * ฟังก์ชันควบคุมหน้าจอ Nextion
 *
 *   Nextion ใช้ protocol แบบ text: ส่ง string แล้วตาม 0xFF 0xFF 0xFF
 *   ตัวอย่าง: ตั้งค่า text widget: "msg0.txt=\"Float\"" + 3×0xFF
 *             ตั้งค่า background: "msg0.bco=WHITE" + 3×0xFF
 * ========================================================================= */

/**
 * @brief  ส่ง terminator ของ Nextion command (0xFF 0xFF 0xFF)
 *         ต้องส่งหลังทุก command ถึงจะมีผล
 *         ไม่หน่วงเพิ่มที่นี่: UART hardware จะ serialize byte ตามลำดับเอง
 */
void nxtSendTerminator(void)
{
    uartPutChar1(0xFF);
    uartPutChar1(0xFF);
    uartPutChar1(0xFF);
}

/**
 * @brief  กำหนดค่าให้ widget ใน Nextion
 *         ตรวจอัตโนมัติว่า widget เป็น .txt หรือเปล่า
 *         ถ้าใช่จะใส่ " " ครอบค่า
 *
 * @param  widget  ชื่อ widget เช่น "msg0.txt" หรือ "j0.val"
 * @param  value   ค่าที่ต้องการส่ง (string)
 *
 * @example  nxtSendText("msg0.txt", "Charge");  → msg0.txt="Charge"
 * @example  nxtSendText("j0.val", "50");        → j0.val=50
 */
void nxtSendText(const char *widget, const char *value)
{
    bool isTxtWidget = false;
    const char *p = widget;

    /* ตรวจว่าชื่อ widget มี ".txt" หรือไม่ */
    while (*p)
    {
        if (p[0]=='t' && p[1]=='x' && p[2]=='t') { isTxtWidget = true; }
        p++;
    }

    while (*widget) { uartPutChar1((uint8_t)*widget++); }
    uartPutChar1('=');
    if (isTxtWidget) { uartPutChar1('"'); }  /* เปิด " สำหรับ .txt widget */
    while (*value)   { uartPutChar1((uint8_t)*value++); }
    if (isTxtWidget) { uartPutChar1('"'); }  /* ปิด " */
    nxtSendTerminator();
}

/**
 * @brief  ซ่อนปุ่มทั้งหมดและโหลด preset เวลาชาร์จจาก EEPROM
 *         เรียกตอนเปิดเครื่องและตอนออกจาก admin mode
 */
void nxtDefaultLayout(void)
{
    uint8_t chargeTimeSlot;
    /* รายชื่อปุ่มที่ต้องซ่อน */
    const char *btns[] = {"b0","b1","b2","b3","b5","b6","b7","b8"};
    uint8_t i;

    /* ซ่อนปุ่มทุกตัว */
    for (i = 0u; i < 8u; i++)
    {
        uartPutStr1("vis "); uartPutStr1(btns[i]); uartPutStr1(",0");
        nxtSendTerminator();
        delayMs((i & 1u) ? 2u : 10u); /* สลับ delay 10ms / 2ms */
    }

    /* โหลด preset เวลาชาร์จจาก EEPROM แล้วไฮไลต์ปุ่มที่ตรงกัน */
    chargeTimeSlot = eepromReadByte(EP_ADDR_CHARGE_TIME);
    if ((chargeTimeSlot < 1u || chargeTimeSlot > 3u) && chargeTimeSlot != 10u)
    {
        chargeTimeSlot = 10u;
        eepromWriteByteIfChanged(EP_ADDR_CHARGE_TIME, 10u);
    }

    switch (chargeTimeSlot)
    {
        case  1: /* 1 ชั่วโมง */
            uartPutStr1("vis b5,1"); nxtSendTerminator(); delayMs(2);
            uartPutStr1("b5.bco=64913"); nxtSendTerminator(); break;
        case  2: /* 2 ชั่วโมง */
            uartPutStr1("vis b6,1"); nxtSendTerminator(); delayMs(2);
            uartPutStr1("b6.bco=64913"); nxtSendTerminator(); break;
        case  3: /* 3 ชั่วโมง */
            uartPutStr1("vis b7,1"); nxtSendTerminator(); delayMs(2);
            uartPutStr1("b7.bco=64913"); nxtSendTerminator(); break;
        case 10: /* ไม่จำกัดเวลา */
            uartPutStr1("vis b8,1"); nxtSendTerminator(); delayMs(2);
            uartPutStr1("b8.bco=64913"); nxtSendTerminator(); break;
        default: break;
    }
}

/**
 * @brief  เปลี่ยน background สี RFID slot ที่เลือกเป็นสีเหลือง
 * @param  slot  หมายเลข slot (1–5)
 */
void nxtHighlightSelected(uint8_t slot)
{
    const char *tLabel[] = {"t1","t2","t3","t4","t5"};
    if (slot < 1u || slot > 5u) { return; }
    uartPutStr1(tLabel[slot - 1u]);
    uartPutStr1(".bco=YELLOW");
    nxtSendTerminator();
}

/** @brief  แสดงเวอร์ชัน firmware บน widget msg3 ของ Nextion */
void nxtShowVersion(void)
{
    nxtSendText("msg3.txt", FW_VERSION);
}

/**
 * @brief  ตั้งความสว่างหน้าจอ Nextion
 * @param  pct  ความสว่าง 0–100 (%)
 */
void nxtSetBrightness(uint8_t pct)
{
    sprintf(atoiBuf, "dim=%u", (unsigned)pct);
    uartPutStr1(atoiBuf);
    nxtSendTerminator();
}

/* =========================================================================
 * ไดรเวอร์ LED WS2812B (NeoPixel)
 *
 *   WS2812B ใช้ 1-wire protocol ที่ time-critical มาก
 *   ส่งข้อมูลสีผ่าน bit-bang แบบ blocking สั้นๆ ใน ws2812Update()
 *   timing รายบิตมาจาก instruction sequence ใน ws2812SendByte()
 *   ลำดับ byte คือ G → R → B (ไม่ใช่ RGB!)
 *
 *   การทำงาน:
 *     1. เรียก ws2812SetAll() หรือแก้ ledArray[] โดยตรง
 *     2. เรียก ws2812Update() เพื่อส่งข้อมูล LED ทั้งหมดทันที
 * ========================================================================= */

/**
 * @brief  ส่ง 1 byte ไปยัง WS2812B ผ่าน bit-bang (MSB ก่อน)
 *         ฟังก์ชันนี้ time-critical และ compiler-sensitive:
 *         ต้องตรวจ disassembly/oscilloscope หากเปลี่ยน clock หรือ optimization
 * @param  val  ค่า byte ที่ส่ง (G, R, หรือ B channel)
 */
void ws2812SendByte(uint8_t val)
{
    int8_t bit;
    for (bit = 7; bit >= 0; bit--)
    {
        WS2812_DAT = 1;  /* เริ่มต้นด้วย High เสมอ */
        if (val & (1u << (uint8_t)bit))
        {
            /* บิต "1": high ยาวขึ้นด้วย legacy DL_8us (8 NOP) */
            WS_T1H();
            WS2812_DAT = 0;
            WS_T1L();
        }
        else
        {
            /* บิต "0": high สั้นจาก overhead แล้ว low ยาวขึ้นด้วย DL_85us */
            WS_T0H();
            WS2812_DAT = 0;
            WS_T0L();
        }
    }
}

/**
 * @brief  เริ่มกระบวนการส่งข้อมูลสีทั้งหมดใน ledArray[] ไปยัง LED strip
 *         ฟังก์ชันนี้ blocking ชั่วครู่และยกระดับ CPU IPL ระหว่างส่งข้อมูล
 *         เพื่อไม่ให้ ISR อื่นแทรกจน timing ของ WS2812B เพี้ยน
 */
void ws2812Update(void)
{
    uint8_t i;
    uint16_t savedIpl;

    WS2812_DAT = 0u;
    delayUs(80u); /* reset/latch low ก่อนเริ่มส่ง frame ใหม่ */

    SET_AND_SAVE_CPU_IPL(savedIpl, 7);
    for (i = 0u; i < LED_COUNT; i++)
    {
        ws2812SendByte(ledArray[i].g); /* WS2812B order: G */
        ws2812SendByte(ledArray[i].r); /* R */
        ws2812SendByte(ledArray[i].b); /* B */
    }
    WS2812_DAT = 0u;
    RESTORE_CPU_IPL(savedIpl);

    delayUs(80u); /* latch หลังส่งข้อมูลครบ */
}

/**
 * @brief  ตั้งสี LED ทุกดวงเป็นสีเดียวกัน แล้วส่งผ่าน ws2812Update()
 * @param  g  ความเข้มสีเขียว (0–255)
 * @param  r  ความเข้มสีแดง   (0–255)
 * @param  b  ความเข้มสีน้ำเงิน (0–255)
 */
void ws2812SetAll(uint8_t g, uint8_t r, uint8_t b)
{
    uint8_t i;
    for (i = 0u; i < LED_COUNT; i++)
    {
        ledArray[i].g = g;
        ledArray[i].r = r;
        ledArray[i].b = b;
    }
    ws2812Update();
}

/* =========================================================================
 * ฟังก์ชัน RFID / PN532
 * ========================================================================= */

/**
 * @brief  ส่ง init sequence ไปยัง PN532 และรอจนกว่า PN532 พร้อมรับคำสั่ง
 *         เรียกตอน startup และทุก 60 วินาที เพื่อป้องกัน PN532 ค้าง
 *         ฟังก์ชันนี้ส่ง read command ท้าย routine แล้วเหมือน v2.04a
 */
static uint8_t pn532InitState = 0u;

static void pn532SendInitCommand(void)
{
    uint8_t i;
    for (i = 0u; i < PN532_INIT_LEN; i++) { uartPutChar2(pn532InitCmd[i]); }
}

static void pn532SendReadCommand(void)
{
    uint8_t i;
    for (i = 0u; i < PN532_READ_LEN; i++) { uartPutChar2(pn532ReadCmd[i]); }
}

void pn532InitAsyncStart(void)
{
    Flag.TagDetF    = 0;
    Flag.TagFoundF  = 0;
    Flag.PN532GotF  = 0;
    msecPn532InitDelay = PN532_INIT_PRE_MS;
    pn532InitState  = 1u;
}

bool pn532InitAsyncBusy(void)
{
    return (pn532InitState != 0u);
}

void pn532InitAsyncService(void)
{
    if (pn532InitState == 0u || msecPn532InitDelay > 0u)
    {
        return;
    }

    switch (pn532InitState)
    {
        case 1u:
            evChargerServiceRx();
            evChargerFlushRfidRx();
            rxIdxRfid      = 0u;
            Flag.PN532RdyF = 0u;
            pn532SendInitCommand();
            msecPn532InitDelay = PN532_INIT_ACK_MS;
            pn532InitState = 2u;
            break;

        case 2u:
            evChargerServiceRx();
            if (Flag.PN532RdyF) { LED_RED = !LED_RED; }
            msecPn532InitDelay = PN532_INIT_SAM_MS;
            pn532InitState = 3u;
            break;

        case 3u:
            evChargerServiceRx();
            evChargerFlushRfidRx();
            rxIdxRfid = 0u;
            pn532SendReadCommand();
            msecPn532InitDelay = PN532_INIT_READ_MS;
            pn532InitState = 4u;
            break;

        default:
            pn532InitState = 0u;
            break;
    }
}

void pn532Init(void)
{
    pn532InitAsyncStart();
    while (pn532InitAsyncBusy())
    {
        evChargerServiceRx();
        pn532InitAsyncService();
    }
}

/** @brief  ส่ง command ให้ PN532 สแกนหาบัตรรอบใหม่ */
void pn532RequestRead(void)
{
    if (pn532InitAsyncBusy()) { return; }
    evChargerServiceRx();
    evChargerFlushRfidRx();
    rxIdxRfid = 0u;
    pn532SendReadCommand();
}

/**
 * @brief  แปลง raw bytes ใน pn532Buf เป็น UID 32 bit
 *
 *   PN532 ส่ง UID ในลำดับ byte: [4]=LSB ... [7]=MSB
 *   ต้องกลับลำดับก่อนรวมเป็น uint32_t
 *
 *   ตัวอย่าง: raw bytes = 5B 99 18 77
 *             UID uint32 = 0x7718995B = 2,000,287,835
 *
 * @return UID 32 bit (0 = ไม่มีข้อมูล)
 */
uint32_t pn532ParseTagId(void)
{
    uint32_t id = (uint32_t)pn532Buf[7]; id <<= 8; /* MSB */
    id |= (uint32_t)pn532Buf[6];         id <<= 8;
    id |= (uint32_t)pn532Buf[5];         id <<= 8;
    id |= (uint32_t)pn532Buf[4];                   /* LSB */
    return id;
}

/**
 * @brief  ตรวจสอบว่า UID ของบัตรมีสิทธิ์ใช้งานตู้ชาร์จนี้หรือไม่
 *         ค้นหาใน tagAdminList[0] ตาม runtime เดิม และ rfidAuth1–5 (บัตรผู้ใช้)
 * @param  tagId  UID ที่ต้องการตรวจสอบ
 * @return true  = บัตรมีสิทธิ์
 *         false = บัตรไม่มีสิทธิ์ หรือ UID = 0
 */
bool rfidIsAuthorised(uint32_t tagId)
{
    if (tagId == 0u) { return false; } /* UID ศูนย์ = ไม่ถูกต้อง */

    /* v2.04a ใช้ admin ใบแรกใน runtime; รายการอื่นคงไว้เป็นข้อมูลเดิม */
    if (tagId == tagAdminList[0]) { return true; }
    /* ตรวจกับ 5 slot ที่บันทึกไว้ใน EEPROM */
    if (tagId == rfidAuth1) { return true; }
    if (tagId == rfidAuth2) { return true; }
    if (tagId == rfidAuth3) { return true; }
    if (tagId == rfidAuth4) { return true; }
    if (tagId == rfidAuth5) { return true; }
    return false;
}

/**
 * @brief  อ่าน UID บัตรทั้ง 5 slot จาก EEPROM แล้วอัปเดตหน้าจอ Nextion
 *         แสดงเฉพาะ 4 หลักสุดท้ายของ UID เพื่อความเป็นส่วนตัว
 *         เรียกตอน startup และหลังบันทึก/ลบบัตร
 */
void rfidRefreshDisplay(void)
{
    uint32_t slotId;
    uint8_t  slot;
    uint16_t addr;
    /* pointer ไปยังตัวแปร rfidAuth1–5 */
    volatile uint32_t *authPtr[EP_RFID_SLOT_COUNT] = {
        &rfidAuth1, &rfidAuth2, &rfidAuth3, &rfidAuth4, &rfidAuth5
    };
    /* ชื่อ widget บน Nextion สำหรับแต่ละ slot */
    const char *widgetLabel[] = {"t1.txt","t2.txt","t3.txt","t4.txt","t5.txt"};

    for (slot = 0u; slot < EP_RFID_SLOT_COUNT; slot++)
    {
        addr   = (uint16_t)(EP_ADDR_RFID_BASE + slot * EP_RFID_SLOT_STRIDE);
        slotId = eepromReadLong(addr);

        if (slotId != 0u && slotId != 0xFFFFFFFFUL)
        {
            /* slot มีบัตรบันทึกอยู่ → อัปเดตตัวแปรและแสดงผล */
            *authPtr[slot] = slotId;

            /* แสดง 4 หลักสุดท้ายของ UID บน Nextion */
            uartPutStr1(widgetLabel[slot]);
            uartPutChar1('='); uartPutChar1('"');
            sprintf(atoiBuf, "%lu", slotId % 10000UL);
            uartPutStr1(atoiBuf);
            uartPutChar1('"');
            nxtSendTerminator();
        }
        else
        {
            /* slot ว่าง → ล้าง RAM และล้างหน้าจอ */
            *authPtr[slot] = 0u;
            nxtSendText(widgetLabel[slot], "");
        }
    }
}

/**
 * @brief  บันทึก UID ใหม่ลง slot 1 โดยเลื่อน slot เก่าลงไปทีละตำแหน่ง
 *         slot 5 (เก่าที่สุด) จะถูกทับทิ้ง
 *
 *   ลำดับการเลื่อน:
 *     slot5 ← slot4 ← slot3 ← slot2 ← slot1 ← tagId (ใหม่)
 *
 * @param  tagId  UID บัตรที่ต้องการบันทึก
 */
void rfidRecordTag(uint32_t tagId)
{
    uint32_t prev;

    /* เลื่อน slot 4→5 (slot 5 เก่าสุดถูกทับ) */
    prev = eepromReadLong(EP_ADDR_RFID_BASE + 3u * EP_RFID_SLOT_STRIDE);
    eepromWriteLongIfChanged(EP_ADDR_RFID_BASE + 4u * EP_RFID_SLOT_STRIDE, prev);

    prev = eepromReadLong(EP_ADDR_RFID_BASE + 2u * EP_RFID_SLOT_STRIDE);
    eepromWriteLongIfChanged(EP_ADDR_RFID_BASE + 3u * EP_RFID_SLOT_STRIDE, prev);

    prev = eepromReadLong(EP_ADDR_RFID_BASE + 1u * EP_RFID_SLOT_STRIDE);
    eepromWriteLongIfChanged(EP_ADDR_RFID_BASE + 2u * EP_RFID_SLOT_STRIDE, prev);

    prev = eepromReadLong(EP_ADDR_RFID_BASE);
    eepromWriteLongIfChanged(EP_ADDR_RFID_BASE + 1u * EP_RFID_SLOT_STRIDE, prev);

    /* เขียน UID ใหม่ลง slot 1 */
    eepromWriteLongIfChanged(EP_ADDR_RFID_BASE, tagId);

    rfidRefreshDisplay(); /* อัปเดตหน้าจอให้ตรงกับ EEPROM */
}

/**
 * @brief  ลบ UID บัตรทั้ง 5 slot ออกจาก EEPROM และล้างหน้าจอ
 *         เรียกเมื่อ admin กดปุ่ม "Erase" (b1)
 */
void rfidEraseAll(void)
{
    uint8_t  slot;
    uint16_t addr;

    /* ล้างตัวแปร RAM ก่อน */
    rfidAuth1 = rfidAuth2 = rfidAuth3 = rfidAuth4 = rfidAuth5 = 0u;

    /* เขียนศูนย์ลงทุก slot ใน EEPROM */
    for (slot = 0u; slot < EP_RFID_SLOT_COUNT; slot++)
    {
        addr = (uint16_t)(EP_ADDR_RFID_BASE + slot * EP_RFID_SLOT_STRIDE);
        eepromWriteLongIfChanged(addr, 0u);
    }
    rfidTagId = 0u;
    rfidRefreshDisplay(); /* อัปเดตหน้าจอให้แสดงว่าว่างแล้ว */
}

/* =========================================================================
 * ฟังก์ชัน decode สัญญาณ CP และแสดงสถานะ
 * ========================================================================= */

/**
 * @brief  แปลงค่า ADC ที่อ่านจากสัญญาณ CP Pilot เป็น enum CpState_t
 *
 *   สัญญาณ CP มีระดับแรงดันหลายระดับตาม IEC 61851-1:
 *     +12V → ไม่มีรถ (state A)
 *     +9V  → รถเสียบ ยังไม่พร้อม (state B)
 *     +6V  → รถพร้อมชาร์จ (state C)
 *     +3V  → รถชาร์จต้องการพัดลม (state C2)
 *     0V / ผิดปกติ → fault (state E/F)
 *
 *   การแปลงนี้ใช้ค่า threshold ที่นิยามใน ev_charger.h
 *
 * @param  adcVal  ค่า ADC ดิบ (0–1023) จาก cpAdcRaw
 * @return CpState_t ที่ตรงกับแรงดัน CP
 */
CpState_t cpDecodePilot(uint16_t adcVal)
{
    if      (adcVal > CP_THRESH_FLOAT)   { return CP_FLOAT;   } /* +12V: ไม่มีรถ   */
    else if (adcVal > CP_THRESH_CONNECT) { return CP_CONNECT; } /* +9V:  รถเสียบ   */
    else if (adcVal > CP_THRESH_CHARGE)  { return CP_CHARGE;  } /* +6V:  ชาร์จได้  */
    else if (adcVal > CP_THRESH_HITEMP)  { return CP_HITEMP;  } /* +3V:  ชาร์จ+พัดลม */
    else                                  { return CP_ERROR;   } /* ผิดปกติ          */
}

/**
 * @brief  อัปเดต widget msg0 บน Nextion ให้แสดงสถานะ CP ปัจจุบัน
 *         พร้อมเปลี่ยนสี background ตามระดับความเร่งด่วน
 * @param  state  สถานะที่ decode แล้วจาก cpDecodePilot()
 */
void cpShowStatus(CpState_t state)
{
    switch (state)
    {
        case CP_FLOAT:
            /* ไม่มีรถ – พื้นขาว ตัวอักษรดำ */
            uartPutStr1("msg0.txt=\"Float\"");   nxtSendTerminator();
            uartPutStr1("msg0.bco=WHITE");        nxtSendTerminator();
            uartPutStr1("msg0.pco=BLACK");        nxtSendTerminator();
            break;
        case CP_CONNECT:
            /* รถเสียบแล้ว ยังไม่ชาร์จ – พื้นเหลือง */
            uartPutStr1("msg0.txt=\"Connect\"");  nxtSendTerminator();
            uartPutStr1("msg0.bco=YELLOW");       nxtSendTerminator();
            uartPutStr1("msg0.pco=BLACK");        nxtSendTerminator();
            break;
        case CP_CHARGE:
            /* กำลังชาร์จ – พื้นน้ำเงิน (30175 = เฉดน้ำเงิน) */
            uartPutStr1("msg0.txt=\"Charge\"");   nxtSendTerminator();
            uartPutStr1("msg0.bco=30175");        nxtSendTerminator();
            uartPutStr1("msg0.pco=BLACK");        nxtSendTerminator();
            break;
        case CP_HITEMP:
            /* ชาร์จแบบมีพัดลม – พื้นเขียว */
            uartPutStr1("msg0.txt=\"Hi temp!\""); nxtSendTerminator();
            uartPutStr1("msg0.bco=GREEN");        nxtSendTerminator();
            uartPutStr1("msg0.pco=BLACK");        nxtSendTerminator();
            break;
        case CP_ERROR:
        default:
            /* ผิดปกติ – พื้นแดง ตัวอักษรขาว (ดูชัดกว่าบนแดง) */
            uartPutStr1("msg0.txt=\"Error!\"");   nxtSendTerminator();
            uartPutStr1("msg0.bco=RED");          nxtSendTerminator();
            uartPutStr1("msg0.pco=WHITE");        nxtSendTerminator();
            break;
    }
}

/* =========================================================================
 * State Machine ของกระบวนการชาร์จ
 * ========================================================================= */

/**
 * @brief  ประมวลผล 1 รอบของ JobStep state machine
 *         เรียกจาก main loop ทุกรอบ หลังจาก cpDecodePilot() แล้ว
 *
 * ตารางการเปลี่ยน state:
 * ┌──────────────┬───────────────────────────┬──────────────────┐
 * │ state ปัจจุบัน │ เงื่อนไข                   │ state ถัดไป      │
 * ├──────────────┼───────────────────────────┼──────────────────┤
 * │ JOB_IDLE     │ Auth2F + CP ≠ Float        │ JOB_CONNECT      │
 * │ JOB_CONNECT  │ CP = Charge หรือ HiTemp    │ JOB_CHARGE       │
 * │ JOB_CONNECT  │ timeout msecTO4 (5s)       │ JOB_IDLE         │
 * │ JOB_CONNECT  │ timeout msecTO5 (30s)      │ JOB_IDLE         │
 * │ JOB_CONNECT  │ Auth2F (แตะบัตรหยุด)       │ JOB_STOP         │
 * │ JOB_CHARGE   │ Auth2F (แตะบัตรหยุด)       │ JOB_STOP         │
 * │ JOB_CHARGE   │ CP = Connect               │ JOB_CONNECT      │
 * │ JOB_CHARGE   │ CP = Float/Error           │ JOB_STOP         │
 * │ JOB_STOP     │ relay ปิดแล้ว              │ JOB_IDLE         │
 * │ JOB_STOP     │ Auth2F (แตะบัตรเริ่มใหม่)  │ JOB_CONNECT      │
 * └──────────────┴───────────────────────────┴──────────────────┘
 *
 * @param  cpState  สถานะ CP ที่ decode แล้วในรอบนี้
 */
void stateMachineRun(CpState_t cpState)
{
    switch (jobStep)
    {
        /* ------------------------------------------------------------------ */
        case JOB_IDLE:
            /* สถานะรอ: ไม่มีรถ หรือรอสแกนบัตร
               LED ภายนอก: เขียว (ปกติ), relay: ปิด, CP: +12V (ไม่ส่ง PWM) */
            LEDX_GRN = 1; LEDX_RED = 0; LEDX_BLU = 0;
            Flag.CPenb = 0; /* ปิด CP PWM: relay ปิด ส่ง +12V แทน */

            if (RELAY_1)
            {
                /* relay ยังเปิดอยู่ทั้งที่ควรปิด – บังคับปิด */
                ws2812SetAll(255u, 0u, 0u); /* LED แดง = relay กำลังปิด */
                msecDim2 = 60000u;
                Flag.relay1Prv = RELAY_1 = 0;
                uartPutStr1("msg4.bco=61309"); nxtSendTerminator(); /* Nextion: สีเทา */
            }
            else if (msecDim2 == 0u)
            {
                /* dim LED ลงหลังจากไม่มีกิจกรรม */
                ws2812SetAll(5u, 0u, 0u); /* เขียวจาง = standby */
                msecDim2 = 60000u;
            }

            /* ตรวจว่ามีบัตรผ่านการ authenticate แล้วหรือไม่ */
            if (Flag.Auth2F)
            {
                Flag.Auth2F = 0;
                if (cpState != CP_FLOAT)
                {
                    /* มีบัตรและมีรถเสียบอยู่ → เปลี่ยนเป็น CONNECT */
                    jobStep  = JOB_CONNECT;
                    msecTO4  = CHARGE_TIMEOUT_MS;   /* 5 วินาที */
                    msecTO5  = CONNECT_TIMEOUT_MS;  /* 30 วินาที */
                }
                /* แนวเดิม v2.04a: ถ้า CP ไม่ใช่ Float ให้เข้า CONNECT */
            }
            break;

        /* ------------------------------------------------------------------ */
        case JOB_CONNECT:
            /* สถานะเชื่อมต่อ: ส่ง CP PWM แจ้ง Imax แต่ยังไม่เปิด relay
               รอรถปิด S2 จน CP เป็น Charge/HiTemp ก่อนจ่าย AC ตาม IEC 61851 */
            LEDX_GRN = 1; LEDX_RED = 1; LEDX_BLU = 1;
            Flag.CPenb = 1; /* เปิด CP PWM */

            if (RELAY_1)
            {
                /* CONNECT ตาม IEC ต้องยังไม่จ่าย AC: ถ้า relay ค้างให้ตัดทันที */
                ws2812SetAll(255u, 0u, 0u);
                msecDim2 = 60000u;
                Flag.relay1Prv = RELAY_1 = 0;
                uartPutStr1("msg4.bco=61309"); nxtSendTerminator();
            }

            /* ตัดสินใจตาม CP state ปัจจุบัน */
            switch (cpState)
            {
                case CP_FLOAT:
                    /* รถถอดสายออกก่อนพร้อมชาร์จ → กลับ IDLE */
                    jobStep = JOB_IDLE;
                    break;
                case CP_CONNECT:
                    /* รถเสียบแต่ยังไม่พร้อม → รอ timeout นานขึ้น */
                    if (msecTO5 == 0u) { jobStep = JOB_IDLE; }
                    break;
                case CP_CHARGE:
                    /* รถพร้อมชาร์จแล้ว → ไป CHARGE ทันที */
                    jobStep = JOB_CHARGE;
                    break;
                case CP_HITEMP:
                    /* รถต้องการพัดลม แต่ชาร์จได้ → ไป CHARGE เช่นกัน */
                    jobStep = JOB_CHARGE;
                    break;
                default:
                    /* CP ผิดปกติ → รอ timeout แล้วกลับ IDLE */
                    if (msecTO4 == 0u) { jobStep = JOB_IDLE; }
                    break;
            }

            /* บัตรแตะขณะชาร์จอยู่ → หยุดชาร์จ */
            if (Flag.Auth2F)
            {
                Flag.Auth2F = 0;
                jobStep     = JOB_STOP;
            }
            break;

        /* ------------------------------------------------------------------ */
        case JOB_CHARGE:
            /* สถานะชาร์จ: relay เปิด CP ส่ง PWM อยู่
               LED ภายนอก: น้ำเงิน */
            LEDX_GRN = 0; LEDX_RED = 0; LEDX_BLU = 1;
            Flag.CPenb = 1;

            /* ตรวจว่า CP หลุดออกจากสถานะชาร์จหรือไม่: relay ต้องตัดเร็ว */
            if (cpState == CP_CONNECT)
            {
                ws2812SetAll(255u, 0u, 0u);
                msecDim2 = 60000u;
                Flag.relay1Prv = RELAY_1 = 0;
                uartPutStr1("msg4.bco=61309"); nxtSendTerminator();
                jobStep = JOB_CONNECT;
                msecTO5 = CONNECT_TIMEOUT_MS;
            }
            else if (cpState != CP_CHARGE && cpState != CP_HITEMP)
            {
                jobStep = JOB_STOP;
            }
            else if (!RELAY_1)
            {
                /* CP พร้อมชาร์จแล้วจึงเปิด relay: tACon ต้องต่ำกว่า 3 วินาที */
                ws2812SetAll(0u, 160u, 255u); /* LED ม่วงน้ำเงิน = กำลังชาร์จ */
                relayCnt++;
                sprintf(atoiBuf, "%lu", relayCnt);
                nxtSendText("msg2.txt", atoiBuf);
                eepromWriteLongIfChanged(EP_ADDR_RELAY_CNT, relayCnt);
                Flag.relay1Prv = RELAY_1 = 1;
                uartPutStr1("msg4.bco=30175"); nxtSendTerminator(); /* Nextion: น้ำเงิน */
            }

            /* บัตรแตะ → หยุดชาร์จตามคำสั่งผู้ใช้ */
            if (Flag.Auth2F)
            {
                Flag.Auth2F = 0;
                jobStep     = JOB_STOP;
            }
            break;

        /* ------------------------------------------------------------------ */
        case JOB_STOP:
            /* สถานะหยุดชาร์จ: กำลังปิด relay และ CP
               LED ภายนอก: เขียว (กลับสู่ปกติ) */
            LEDX_GRN = 1; LEDX_RED = 0; LEDX_BLU = 0;
            Flag.CPenb = 0; /* ปิด CP PWM ทันที */

            if (RELAY_1)
            {
                /* relay ยังเปิดอยู่ → ปิดและกลับ IDLE */
                ws2812SetAll(255u, 0u, 0u); /* LED แดง ชั่วคราว */
                msecDim2 = 60000u;
                Flag.relay1Prv = RELAY_1 = 0;
                uartPutStr1("msg4.bco=61309"); nxtSendTerminator(); /* Nextion: เทา */
                jobStep = JOB_IDLE; /* กลับ IDLE หลัง relay ปิด */
            }

            /* ผู้ใช้แตะบัตรอีกครั้ง (ต้องการชาร์จต่อ) */
            if (Flag.Auth2F)
            {
                Flag.Auth2F = 0;
                if (cpState != CP_FLOAT)
                {
                    /* แนวเดิม v2.04a: ถ้า CP ไม่ใช่ Float ให้เริ่มใหม่ */
                    jobStep  = JOB_CONNECT;
                    msecTO4  = CHARGE_TIMEOUT_MS;
                    msecTO5  = CONNECT_TIMEOUT_MS;
                }
            }
            break;

        /* ------------------------------------------------------------------ */
        default:
            /* state ไม่รู้จัก → reset กลับ IDLE ปลอดภัย */
            jobStep = JOB_IDLE;
            break;
    }
}

/* =========================================================================
 * CRC-16 / CCITT-FALSE
 *   ใช้สำหรับคำนวณ checksum ของ QR Code PromptPay
 *   polynomial: x^16 + x^12 + x^5 + 1 (0x1021)
 *   initial value: 0xFFFF
 * ========================================================================= */

/**
 * @brief  คำนวณ CRC-16/CCITT-FALSE ของข้อมูล
 * @param  data  pointer ไปยัง array ข้อมูล
 * @param  len   จำนวน byte ที่คำนวณ
 * @return ค่า CRC 16 bit
 */
uint16_t crc16CcittFalse(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu; /* initial value */
    uint16_t i, j;
    for (i = 0u; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8; /* XOR byte เข้า high byte ของ CRC */
        for (j = 0u; j < 8u; j++)      /* ประมวลผลทีละ bit */
        {
            if (crc & 0x8000u)
            {
                crc = (crc << 1) ^ 0x1021u; /* bit สูง = 1: shift และ XOR polynomial */
            }
            else
            {
                crc <<= 1; /* bit สูง = 0: shift เฉยๆ */
            }
        }
    }
    return crc;
}
