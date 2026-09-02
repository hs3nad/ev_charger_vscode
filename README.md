# EV Charger Firmware – DIPROM EVSE 1.0

Firmware สำหรับเครื่องชาร์จรถยนต์ไฟฟ้า บน **PIC24FJ128GA106**  
ปรับปรุงจาก v2.04a → v2.04r พร้อม comment ภาษาไทยครบถ้วน

---

## โครงสร้างโปรเจค

```
ev_charger_diprom/
│
├── .vscode/
│   ├── c_cpp_properties.json   ← IntelliSense config (แก้ path XC16)
│   ├── tasks.json              ← Build / Program tasks
│   ├── launch.json             ← Debug config
│   ├── settings.json           ← VSCode settings
│   └── extensions.json         ← Extension ที่แนะนำ
│
├── include/
│   ├── ev_charger.h            ← ค่าคงที่, enum, ประกาศ function ทั้งหมด
│   └── tahoma font.h           ← ⚠️ คัดลอกจาก project เดิม
│
├── src/
│   ├── ev_charger.c            ← Driver functions (I2C, UART, RFID, CP, WS2812)
│   └── ev_charger_isr_main.c   ← ISR ทุกตัว + main()
│
├── docs/
│   ├── implementation_notes.md ← แนวทาง design/refactor ที่ปรับแล้ว
│   └── remaining_work.md       ← รายการที่ต้องวัด/ยืนยันกับบอร์ดจริง
│
├── CMakeLists.txt              ← Build script (CMake + XC16)
├── .gitignore
└── README.md
```

---

## ขั้นตอนการติดตั้ง

### ขั้นที่ 1: ติดตั้ง Software

| Software | ดาวน์โหลด | หมายเหตุ |
|----------|-----------|----------|
| **VSCode** | [code.visualstudio.com](https://code.visualstudio.com) | version 1.85+ |
| **MPLAB XC16** | [microchip.com/xc16](https://www.microchip.com/en-us/tools-resources/develop/mplab-xc-compilers/xc16) | Free license พอสำหรับโปรเจคนี้ |
| **CMake** | [cmake.org](https://cmake.org/download/) | version 3.21+ |
| **Ninja** | [github.com/ninja-build](https://github.com/ninja-build/ninja/releases) | build เร็วกว่า Make |

### ขั้นที่ 2: ติดตั้ง VSCode Extension

เปิด VSCode แล้วกด `Ctrl+P` แล้วพิมพ์ทีละบรรทัด:

```
ext install Microchip.mplab-extension-pack
ext install ms-vscode.cpptools
ext install ms-vscode.cmake-tools
ext install ms-vscode.serial-monitor
```

หรือเปิดโปรเจคแล้ว VSCode จะถามให้ติดตั้งอัตโนมัติ

### ขั้นที่ 3: คัดลอก header จาก project เดิม

คัดลอกไฟล์นี้จาก MPLAB X project เดิมมาวางใน `include/`:
```
tahoma font.h
```

### ขั้นที่ 4: แก้ path XC16 ในไฟล์ config

แก้ **3 ที่** ให้ตรงกับ XC16 version และ OS ของคุณ:

**1. `.vscode/c_cpp_properties.json`** บรรทัด `compilerPath` และ `includePath`

**2. `.vscode/tasks.json`** บรรทัด `XC16_BIN` และ `IPE_PATH`

**3. `.vscode/settings.json`** บรรทัด `terminal.integrated.env.*`

```
# Windows (ตัวอย่าง)
C:/Program Files/Microchip/xc16/v2.10/bin

# Linux / macOS (ตัวอย่าง)
/opt/microchip/xc16/v2.10/bin
```

---

## วิธี Build

### ครั้งแรก (Configure + Build)

```
Ctrl+Shift+B  →  เลือก "Configure + Build"
```

หรือ Terminal:
```bash
cmake -S . -B build -G Ninja -DXC16_PATH="C:/Program Files/Microchip/xc16/v2.10/bin"
cmake --build build
```

### Build ปกติ (หลังจาก Configure แล้ว)

```
Ctrl+Shift+B
```

### ผลลัพธ์

```
build/
├── ev_charger_diprom        ← ELF file (สำหรับ debug)
└── ev_charger_diprom.hex    ← HEX file (สำหรับ flash ลง MCU)
```

---

## วิธี Flash ลง MCU

### ผ่าน VSCode Task

```
Ctrl+Shift+P  →  "Tasks: Run Task"  →  "Program Device (PICkit 4)"
```

### ผ่าน MPLAB IPE (command line)

```bash
ipecmd.exe -TPPK4 -P24FJ128GA106 -Fbuild/ev_charger_diprom.hex -M -OL
```

---

## วิธี Debug

1. ต่อ PICkit 4 เข้ากับบอร์ด DIPROM EVSE
2. กด **F5** หรือ Run → Start Debugging
3. เลือก **"Debug – MPLAB Adapter (PICkit 4)"**

### Breakpoint ที่แนะนำ

| ไฟล์ | Function | จุดสังเกต |
|------|----------|-----------|
| `ev_charger_isr_main.c` | `stateMachineRun()` | ตรวจ state transitions |
| `ev_charger_isr_main.c` | `evChargerServiceRx()` | ตรวจการ parse Nextion/RFID หลังออกจาก UART ISR |
| `ev_charger.c` | `cpDecodePilot()` | ตรวจค่า ADC |

---

## เอกสารประกอบการรีวิว

- `docs/implementation_notes.md` สรุปแนวทางที่ปรับไปแล้ว เช่น CP hardware PWM, ADC trigger, ISR policy, UART queue, PN532 async, EEPROM ACK polling และ IEC state machine
- `docs/remaining_work.md` เก็บรายการที่ยังต้องวัด/ยืนยันกับบอร์ดจริง เช่น CP waveform, ADC threshold, relay timing และ EEPROM power-cycle test

---

## ดู UART Output (Serial Monitor)

เปิด Serial Monitor ใน VSCode:
- Extension: `ms-vscode.serial-monitor`
- Port: COM port ของ USB-UART converter
- Baud rate: **9600** (UART1 / Nextion)

---

## สรุปการปรับปรุงจาก v2.04a

| หัวข้อ | ก่อน | หลัง |
|--------|------|------|
| ชื่อตัวแปร | `i,j,k,m,n,o...` | `cpAdcRaw`, `jobStep`, `rfidTagId` |
| Magic numbers | `814`, `50`, `123` | `CP_THRESH_FLOAT`, `PWM_DUTY_DEFAULT` |
| WS2812B driver | 300 บรรทัด copy-paste | `ws2812SendByte()` loop 30 บรรทัด |
| Buffer guard | ไม่มี (crash ได้) | `if (rxIdx >= 2u)` ทุกที่ |
| State enum | `Jobstep = 0,1,2,3` | `JOB_IDLE`, `JOB_CONNECT`, `JOB_CHARGE`, `JOB_STOP` |
| TagAdmin overflow | `20036383907` > uint32 | แก้เป็น `2003638390UL` |
| Test bypass | Auth bypass อยู่ตลอด | ลบออกแล้ว (production-safe) |
| Comment | ภาษาอังกฤษบางส่วน | ภาษาไทยครบทุก function และ block |
| CP PWM | software PWM/Timer1 ISR | hardware OC1 บน RB4/RP4, Timer2 1 kHz |
| CP ADC timing | อ่านจาก software timing | Timer3 trigger align กับช่วง High ของ CP PWM |
| UART RX ISR | parse Nextion/RFID ใน interrupt | ISR รับเข้า queue, parse ใน `evChargerServiceRx()` |
| PN532 | init/rearm มี delay blocking | async state machine + counter rearm |
| EEPROM | write แล้ว delay คงที่ | อ่าน ACK จริง + ACK polling + write-if-changed |
| IEC relay control | relay มี path เปิดก่อนรถพร้อม | เปิด relay เฉพาะ CP state C/D และตัดทันทีเมื่อกลับ B/fault |

---

## Hardware Reference

```
PIC24FJ128GA106 Pin Mapping
─────────────────────────────────────────
UART1 RX  ← RP21 (RG6)   ← Nextion TX
UART1 TX  → RP26 (RG7)   → Nextion RX
UART2 RX  ← RP29 (RB15)  ← PN532 TX
UART2 TX  → RP9  (RB9)   → PN532 RX
ADC AN8   ← RB8           ← CP Signal
RELAY     → RB6            → Relay coil
PWM_CP    → RB4            → CP Pilot output
WS2812B   → RF3            → LED strip DIN
I2C SDA   ↔ RB3            ↔ EEPROM SDA
I2C SCL   → RB5            → EEPROM SCL
LED R/Y/G → RB2/RB1/RB0   → On-board LEDs
LEDX RGB  → RD10/RC13/RD1 → Chassis LEDs
BUZZER    → RG8            → Buzzer
SW_LEFT   ← RC12           ← Left button
SW_RIGHT  ← RC15           ← Right button
─────────────────────────────────────────
```
