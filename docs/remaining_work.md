# รายการที่ยังต้องทำ/ยืนยันจากเอกสาร IEC 61851 และวงจร CP

อัปเดตล่าสุด: 2026-05-21

เอกสารอ้างอิงในโปรเจค:
- `IEC 61851-1-2017 PDF.pdf`
- `IEC 61851 timing diagram.jpg`
- `การทำงานของวงจรสร้าง CP.pdf`
- `docs/implementation_notes.md` สรุปแนวทาง firmware ที่ปรับไปแล้ว

## สถานะที่ปรับแล้ว

- [x] ลำดับ `JOB_CONNECT` ปรับให้ส่ง CP PWM ก่อน แต่ยังไม่เปิด relay
- [x] เปิด relay เฉพาะเมื่อ CP เป็น `CP_CHARGE` หรือ `CP_HITEMP`
- [x] เมื่อตอนชาร์จ CP กลับเป็น `CP_CONNECT` จะตัด relay ทันที แล้วกลับไป `JOB_CONNECT`
- [x] เอาการรอ CP หลุด 3 วินาทีออกจาก path ชาร์จ
- [x] แก้ comment duty/current ตาม IEC: ช่วง 10-85% ใช้ `Imax = duty x 0.6 A`
- [x] แก้ comment ระดับแรงดัน CP ที่ขา ADC ตามเอกสารวงจร CP
- [x] ถอด Timer1 ISR และย้าย CP PWM ไปใช้ OC1 hardware PWM
- [x] ใช้ Timer3 เป็น ADC trigger เท่านั้น ไม่ใช้ Timer3 ISR ส่ง WS2812
- [x] ย้าย UART RX parsing ออกจาก ISR ไปทำใน `evChargerServiceRx()`
- [x] เปลี่ยน PN532 reinit/read rearm และเสียง auth beep เป็นแนวทาง non-blocking
- [x] ปรับ EEPROM I2C ให้ตรวจ ACK จริงและใช้ ACK polling หลัง write

## รายการที่ยังไม่ได้ทำ

- [ ] วัด `AVDD/Vref` จริงของ ADC บนบอร์ด
  - เหตุผล: threshold ปัจจุบันเป็นค่า legacy ADC count
  - ต้องใช้เพื่อคำนวณ/ยืนยันค่า `CP_THRESH_FLOAT`, `CP_THRESH_CONNECT`, `CP_THRESH_CHARGE`, `CP_THRESH_HITEMP`

- [ ] วัดค่า ADC จริงของ CP แต่ละ state บนบอร์ด
  - วัด state A: CP +12 V
  - วัด state B: CP +9 V
  - วัด state C: CP +6 V
  - วัด state D: CP +3 V
  - วัดช่วง CP -12 V ที่ขา ADC ต้องไม่ติดลบและใกล้ค่าจากเอกสารวงจร

- [ ] ปรับ threshold หลังมีค่าจริงจากบอร์ด
  - เอกสารวงจรคาดแรงดันที่ขา ADC โดยประมาณ:
    - `+12 V -> 3.6 V`
    - `+9 V -> 3.2 V`
    - `+6 V -> 2.8 V`
    - `+3 V -> 2.4 V`
    - `-12 V -> 0.6 V`
  - ต้องแปลงเป็น ADC count ตาม `AVDD/Vref` จริง

- [ ] วัด CP PWM ที่หัวชาร์จด้วย oscilloscope
  - ความถี่ต้องใกล้ 1 kHz
  - duty ต้องตรงกับค่าที่ตั้งบน Nextion/EEPROM
  - ระดับแรงดันที่หัวชาร์จควรเป็นประมาณ `+12 V / -12 V`
  - ตรวจ rise/fall time เทียบ IEC Table A.11

- [ ] วัด timing relay ON หลัง CP เปลี่ยน `B -> C/D`
  - IEC Sequence 4 ระบุ EVSE ต้อง energize ภายใน `max 3 s`
  - โค้ดสั่งเปิดทันทีเมื่อ decode เป็น `CP_CHARGE`/`CP_HITEMP`
  - ต้องวัดเวลาจริงรวม ADC trigger, main loop, relay driver และ contactor

- [ ] วัด timing relay OFF หลัง CP เปลี่ยน `C/D -> B`
  - IEC Sequence 8.1 ระบุ switching device ต้อง open ภายใน `max 100 ms`
  - โค้ดสั่งตัดทันทีเมื่อ decode เป็น `CP_CONNECT`
  - ต้องวัดเวลาจริงด้วย scope หรือ logic analyzer

- [ ] ยืนยัน rating กระแสของตู้/สาย/contactor ก่อนล็อกค่า duty
  - ตอนนี้ช่วงตั้งค่า `40-60%` เทียบ IEC คือประมาณ `24-36 A`
  - ค่า default `50%` เทียบ IEC คือ `30 A`
  - ถ้าตู้รองรับ 25 A จริง ควรพิจารณา default ประมาณ `42%` หรือจำกัด max ใหม่

- [ ] ตรวจ behavior เมื่อ CP เป็น fault/error
  - CP short/ต่ำผิดปกติควรตัด relay
  - CP open/รถถอดควรกลับ idle อย่างปลอดภัย
  - ต้องทดสอบกับวงจรจำลอง resistor/diode ตาม IEC Annex A

- [ ] ตรวจว่าการ reinit PN532 แบบ async ไม่รบกวน timing CP/relay
  - โดยเฉพาะระหว่างกำลังชาร์จ
  - ยืนยันว่า main loop ยังตอบสนอง CP state ได้เร็วพอ

- [ ] ทดสอบ EEPROM write หลังปรับ ACK polling
  - เขียน/อ่าน `pwmDuty`
  - เขียน/อ่าน RFID slots
  - เขียน/อ่าน `relayCnt`
  - ทดสอบ power cycle หลังเขียน

## หมายเหตุ

รายการด้านบนเป็นงานยืนยันกับบอร์ดจริง เพราะเกี่ยวข้องกับแรงดัน, ADC count, relay delay และ waveform จริง ซึ่งจำลองจากโค้ดอย่างเดียวไม่พอสำหรับความปลอดภัยของ EVSE
