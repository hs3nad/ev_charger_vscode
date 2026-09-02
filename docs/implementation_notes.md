# แนวทางการปรับปรุง Firmware

อัปเดตล่าสุด: 2026-05-21

เอกสารนี้สรุปแนวทางที่ใช้ปรับ firmware ชุดนี้จาก code เดิม โดยตั้งใจให้ใกล้พฤติกรรมเดิมมากที่สุด แต่ย้ายงานที่เกี่ยวกับ timing/safety ไปพึ่ง hardware และลดงานใน interrupt/main loop ที่ไม่จำเป็น

เอกสารอ้างอิงในโปรเจค:
- `IEC 61851-1-2017 PDF.pdf`
- `IEC 61851 timing diagram.jpg`
- `การทำงานของวงจรสร้าง CP.pdf`

## หลักการที่ใช้

- คงพฤติกรรมเดิมไว้เท่าที่ไม่ขัดกับ timing และความปลอดภัยของ EVSE
- งานที่ต้องแม่นเวลาให้ใช้ hardware เป็นหลัก เช่น Output Compare, Timer และ ADC trigger
- ISR ควรทำเฉพาะงานสั้นและจำเป็น ห้าม delay, ห้ามเขียน EEPROM, ห้าม parse protocol ยาวๆ
- Main loop ในช่วงไม่มี event ควรวิ่งสั้นและไม่ block เพื่อให้ตอบสนอง CP/RFID/UI ได้เร็ว
- ค่าที่เกี่ยวกับแรงดันจริง, ADC threshold, relay delay และ waveform ต้องยืนยันกับบอร์ดจริง

## CP PWM และ ADC

- CP PWM ย้ายจาก software PWM/Timer1 ISR ไปใช้ hardware OC1 ที่ RB4/RP4
- Timer2 เป็น time base หลักของ CP PWM ที่ 1 kHz และยังใช้เป็น 1 ms system tick
- Timer3 ไม่เปิด ISR แล้ว ใช้เฉพาะ compare event เพื่อ trigger ADC ให้อ่าน CP ในช่วง High ของ PWM
- ตอนเปิด CP PWM จะ align Timer3 กับ Timer2/OC1 เพื่อให้ตำแหน่ง ADC sample ไม่ drift ไปเรื่อยๆ
- จุด trigger ปัจจุบันวางไว้ประมาณ 300 us หลังเริ่มคาบ PWM เพื่อให้อยู่ในช่วง High ของ duty 40-60%
- ADC อ่าน CP จาก AN8/RB8 แล้ว `cpDecodePilot()` แปลงเป็น state A/B/C/D ตาม threshold เดิมก่อน

ค่าจากเอกสารวงจร CP ที่ต้องใช้ตอน calibrate:
- CP `+12 V` คาดที่ขา ADC ประมาณ `3.6 V`
- CP `+9 V` คาดที่ขา ADC ประมาณ `3.2 V`
- CP `+6 V` คาดที่ขา ADC ประมาณ `2.8 V`
- CP `+3 V` คาดที่ขา ADC ประมาณ `2.4 V`
- CP `-12 V` คาดที่ขา ADC ประมาณ `0.6 V`

หมายเหตุ: threshold ใน code ยังเป็น legacy ADC count จึงต้องวัด `AVDD/Vref` และค่า ADC จริงก่อนปรับเป็นค่าคำนวณใหม่

## Duty และกระแสตาม IEC 61851

- ช่วง duty 10-85% ใช้ความสัมพันธ์ `Imax = duty(%) x 0.6 A`
- ค่า default ปัจจุบัน `50%` เทียบได้ประมาณ `30 A`
- ช่วงตั้งค่าเดิม `40-60%` เทียบได้ประมาณ `24-36 A`
- ก่อนล็อกค่า default/max ควรยืนยัน rating ของสาย, contactor, breaker และหัวชาร์จจริง

## State Machine ตาม IEC 61851

แนวทางปัจจุบัน:
- `JOB_IDLE`: relay ปิด, CP เป็น idle/ไม่จ่าย PWM ตามพฤติกรรมเดิม
- `JOB_CONNECT`: หลัง auth แล้วเริ่มส่ง CP PWM แต่ relay ยังปิด รอรถปิด S2 จน CP เป็น `CP_CHARGE` หรือ `CP_HITEMP`
- `JOB_CHARGE`: เปิด relay เฉพาะหลัง CP เป็น state C/D แล้ว
- ถ้ากำลังชาร์จแล้ว CP กลับเป็น `CP_CONNECT` ให้ตัด relay ทันทีและกลับ `JOB_CONNECT`
- ถ้ากำลังชาร์จแล้ว CP เป็น `CP_FLOAT` หรือ fault/error ให้ไป `JOB_STOP`
- เอาการรอ CP หลุด 3 วินาทีออกจาก path ชาร์จ เพราะเอกสาร IEC ระบุกรณี C/D -> B ต้องเปิด switching device ภายในเวลาสั้นมาก

จุดที่ต้องวัดจริง:
- `B -> C/D` ถึง relay ON ต้องไม่เกินกรอบเวลาที่ IEC กำหนด
- `C/D -> B` ถึง relay OFF ต้องเร็วพอ โดยเฉพาะกรอบ `max 100 ms` ใน IEC 61851-1:2017
- relay/contactor มี delay ทางกล จึงต้องวัดที่ output จริง ไม่ใช่ดูจาก code อย่างเดียว

## Timer และ ISR

แนวทางล่าสุด:
- Timer1 ISR ถูกถอดออก เพราะ interrupt ทุก 100 us ถี่เกินไปและไม่จำเป็นหลังใช้ hardware PWM/ADC trigger
- Timer2 ISR คงไว้ที่ 1 ms เพื่อเป็น system tick, countdown timer, buzzer tempo และ sync service ของ CP PWM
- Timer3 ISR ถูกถอดออก ไม่ใช้ส่ง WS2812 แล้ว ใช้ Timer3 เป็น ADC trigger เท่านั้น
- ADC ISR ทำเฉพาะอ่าน `ADC1BUF0`, เก็บ `cpAdcRaw` และ clear flag
- UART RX ISR ทำเฉพาะรับ byte เข้า queue แล้วออกทันที

หลักคิดคือ interrupt ต้องสั้นและคาดเดาเวลาได้ ส่วนงาน parse, update UI, reinit module และ logic ยาวๆ ให้ทำใน main context

## UART, Nextion และ RFID

- UART1 RX จาก Nextion และ UART2 RX จาก PN532 ถูกเปลี่ยนเป็น ring queue
- การ parse protocol ย้ายออกไปที่ `evChargerServiceRx()` ซึ่งถูกเรียกใน main loop และระหว่าง `delayMs()` เพื่อคงพฤติกรรมเดิม
- Nextion numeric response รับเฉพาะ frame เต็มรูปแบบ `0x71 + 4 data bytes + 0xFF 0xFF 0xFF`
- Event จาก Nextion เปลี่ยนเป็น exact match ลดโอกาส byte ขยะทำให้ flag ผิด
- RFID parser เก็บ payload ของ PN532 ให้ครบก่อน แล้วค่อย set flag เพื่อไม่ให้ byte ใน payload ไป trigger logic อื่น

ผลที่ต้องการคือ UART ISR ไม่ block และไม่ parse งานยาว แต่ยังคงการตอบสนองของ UI/RFID ไว้

## PN532

- `pn532Init()` เดิมเป็น blocking sequence ถูกแยกเป็น state machine:
  - `pn532InitAsyncStart()`
  - `pn532InitAsyncService()`
  - `pn532InitAsyncBusy()`
- `pn532Init()` ยังเป็น blocking wrapper สำหรับช่วง startup เพื่อคง interface เดิม
- การ re-arm คำสั่งอ่านบัตรหลังพบ tag ใช้ counter `msecRfidReadDelay` แทน delay blocking
- การ reinit ทุก 60 วินาทียังอยู่ แต่เดินแบบ async เพื่อลดผลกระทบต่อ CP/relay timing

## เสียง Beep และงาน Blocking

- เสียง feedback หลัง auth ผ่าน/หยุดชาร์จเปลี่ยนเป็น non-blocking state machine
- ใช้ `msecAuthBeepDelay` และ `authBeepService()` แทนการ `delayMs()` ต่อเนื่องใน main path
- `delayMs()` ยังมีอยู่เพื่อคง code เดิมและใช้ใน startup/admin UI บางจุด แต่ภายใน delay จะ service UART/PN532/auth beep ไปด้วย
- งานที่ยังเหลือ delay ชัดเจนควรจำกัดไว้ในช่วงที่ไม่กระทบ safety timing หรือแยก refactor รอบถัดไป

## WS2812B

- ไม่ใช้ Timer3 ISR ยิง LED แล้ว
- `ws2812Update()` ส่งข้อมูล bit-bang แบบ blocking สั้นๆ ใน main context
- ระหว่างส่ง WS2812 จะยกระดับ IPL เป็น 7 ชั่วคราวเพื่อคุม timing ราย bit
- ยังไม่เปลี่ยนเป็น SPI encoding เพราะผู้ใช้ต้องการคงขาเดิม และยังไม่มีการย้าย hardware pin

ข้อควรระวัง: ต้องวัด waveform จริงด้วย scope ถ้าต้องการยืนยัน timing WS2812 บน XC16/optimization ที่ใช้จริง

## EEPROM

- ใช้ external I2C EEPROM ตามแนวทางเดิม ไม่ใช้ Flash ภายใน MCU แทน EEPROM
- เหตุผลคือ PIC24FJ128GA106 ไม่มี data EEPROM แยกแบบที่เหมาะกับงานนี้ และการใช้ Flash emulation มีเรื่อง endurance/erase row/ความเสี่ยงตอน power loss
- I2C bit-bang ปรับให้อ่าน ACK จริงจาก EEPROM
- หลัง write ใช้ ACK polling ตามแนวทาง 24XX256 แทนการ delay คงที่ยาวๆ
- การเขียนค่าซ้ำลดลงด้วย `eepromWriteByteIfChanged()` และ `eepromWriteLongIfChanged()`
- `rfidRefreshDisplay()` ไม่เขียน slot กลับ EEPROM ระหว่าง refresh แล้ว เพื่อลด wear

การทดสอบที่ต้องทำจริงคือ write/read ค่า `pwmDuty`, RFID slots, `relayCnt` และ power cycle หลังเขียน

## Main Loop และ UI

- Main loop ถูกจัดให้ service งานสั้นๆ ต่อรอบ:
  - service UART queue
  - service PN532 async init/reinit
  - service auth beep
  - re-arm RFID เมื่อ counter หมด
  - update CP/UI เฉพาะเมื่อ state เปลี่ยนหรือถึงรอบ update
  - poll ค่า PWM จาก Nextion เป็นช่วงๆ แบบไม่ block
- การ update Nextion ที่ไม่จำเป็นทุก loop ลดลง เพื่อลดเวลาวิ่งรอบปกติ
- ค่า ADC raw debug แสดงเป็นช่วงๆ ไม่ยิงทุก loop

เป้าหมายคือรอบปกติเมื่อไม่มี event ควรอยู่ประมาณ 1-2 ms หรือน้อยกว่า แต่ยังต้องวัดจริงบนบอร์ด/logic analyzer

## สิ่งที่ตั้งใจยังไม่เปลี่ยน

- ค่า ADC threshold ยังไม่ recalibrate จนกว่าจะมีค่าจากบอร์ดจริง
- default PWM ยังเป็น 50% เพื่อไม่เปลี่ยนพฤติกรรมเดิมก่อนยืนยัน rating
- WS2812 ยังเป็น bit-bang บนขาเดิม
- โครงสร้างไฟล์ `ev_charger.c` และ `ev_charger_isr_main.c` ยังแยกไว้ก่อน เพื่อเน้นทำให้ code ทำงานจริงและตรวจง่าย
- `delayMs()` ยังไม่ถูกลบทั้งหมด เพราะบางจุดเป็น startup/admin UI และต้องแยก refactor อย่างระมัดระวัง

## รายการทดสอบต่อ

รายการที่ต้องยืนยันกับบอร์ดจริงเก็บไว้ที่ `docs/remaining_work.md` โดยเฉพาะ:
- CP PWM frequency/duty/voltage ที่หัวชาร์จ
- ADC trigger alignment กับช่วง High ของ CP
- ADC threshold จริงของ state A/B/C/D
- relay ON/OFF timing เทียบ IEC
- EEPROM write/read และ power cycle
- PN532 async reinit ระหว่าง runtime
