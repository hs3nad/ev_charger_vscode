# =============================================================================
# CMakeLists.txt  –  EV Charger Firmware (DIPROM EVSE 1.0)
# =============================================================================
#
# วิธีใช้:
#   1. ติดตั้ง Extension Pack "MPLAB" จาก Microchip ใน VSCode
#   2. ติดตั้ง MPLAB XC16 compiler (https://www.microchip.com/en-us/tools-resources/develop/mplab-xc-compilers)
#   3. เปิดโฟลเดอร์นี้ใน VSCode → Extension MPLAB จะตรวจพบ CMakeLists.txt
#   4. กด "Build" หรือ Ctrl+Shift+B
#
# ไฟล์นี้สร้างโดยใช้ CMake แบบ cross-compile สำหรับ PIC24FJ128GA106
# XC16 เป็น GCC variant ดังนั้นใช้ CMAKE_C_COMPILER ชี้ไปที่ xc16-gcc
# =============================================================================

cmake_minimum_required(VERSION 3.21)

# -----------------------------------------------------------------------------
# ชื่อโปรเจคและภาษาที่ใช้
# -----------------------------------------------------------------------------
project(ev_charger_diprom
    VERSION     2.4.0
    DESCRIPTION "EV Charger Firmware – PCB DIPROM EVSE 1.0 (PIC24FJ128GA106)"
    LANGUAGES   C ASM
)

# -----------------------------------------------------------------------------
# เป้าหมาย MCU – PIC24FJ128GA106
#   MCU_NAME   : ชื่อที่ส่งให้ xc16-gcc ด้วย -mcpu=
#   MCU_FAMILY : ใช้กำหนด include path ของ Microchip header
# -----------------------------------------------------------------------------
set(MCU_NAME    "24FJ128GA106")
set(MCU_FAMILY  "PIC24F")

# -----------------------------------------------------------------------------
# ค้นหา XC16 compiler
#   ค้นหาใน path ปกติก่อน; ถ้าไม่เจอให้ระบุ XC16_PATH เองผ่าน cmake -D
# -----------------------------------------------------------------------------
if(NOT XC16_PATH)
    # Windows: C:/Program Files/Microchip/xc16/v2.xx/bin
    # macOS  : /opt/microchip/xc16/v2.xx/bin
    # Linux  : /opt/microchip/xc16/v2.xx/bin
    find_program(XC16_GCC
        NAMES xc16-gcc
        PATHS
            "C:/Program Files/Microchip/xc16/v2.10/bin"
            "C:/Program Files/Microchip/xc16/v2.00/bin"
            "/opt/microchip/xc16/v2.10/bin"
            "/opt/microchip/xc16/v2.00/bin"
        DOC "Path to xc16-gcc executable"
    )
    if(NOT XC16_GCC)
        message(FATAL_ERROR
            "ไม่พบ xc16-gcc – ระบุ path ด้วย: cmake -DXC16_PATH=/opt/microchip/xc16/vX.XX/bin")
    endif()
    get_filename_component(XC16_PATH "${XC16_GCC}" DIRECTORY)
endif()

message(STATUS "XC16 path: ${XC16_PATH}")

set(CMAKE_C_COMPILER   "${XC16_PATH}/xc16-gcc")
set(CMAKE_ASM_COMPILER "${XC16_PATH}/xc16-as")
set(CMAKE_LINKER       "${XC16_PATH}/xc16-ld")
set(CMAKE_AR           "${XC16_PATH}/xc16-ar")
set(CMAKE_OBJCOPY      "${XC16_PATH}/xc16-objcopy")
find_program(XC16_SIZE
    NAMES xc16-size size
    PATHS "${XC16_PATH}"
    NO_DEFAULT_PATH
)

# ป้องกัน CMake test-compile บน host ที่ไม่ใช่ PIC (cross-compile)
set(CMAKE_C_COMPILER_FORCED TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# -----------------------------------------------------------------------------
# Include path สำหรับ Microchip device header
#   xc16 เก็บ header ไว้ที่:  <xc16_install>/support/PIC24F/h/
#   และ generic header ที่:   <xc16_install>/support/generic/h/
# -----------------------------------------------------------------------------
get_filename_component(XC16_ROOT "${XC16_PATH}" DIRECTORY)  # ขึ้น 1 ระดับจาก bin/

set(XC16_INCLUDE_DIRS
    "${XC16_ROOT}/support/${MCU_FAMILY}/h"
    "${XC16_ROOT}/support/generic/h"
    "${XC16_ROOT}/support/PIC24F/gld"
)

# -----------------------------------------------------------------------------
# Linker script สำหรับ PIC24FJ128GA106
#   XC16 มาพร้อม linker script ในชื่อ  p24FJ128GA106.gld
# -----------------------------------------------------------------------------
set(LINKER_SCRIPT "${XC16_ROOT}/support/PIC24F/gld/p24FJ128GA106.gld")

# -----------------------------------------------------------------------------
# ไฟล์ source code
# -----------------------------------------------------------------------------
set(SOURCES
    src/ev_charger.c
    src/ev_charger_isr_main.c
)

# -----------------------------------------------------------------------------
# สร้าง executable target
# -----------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${SOURCES})

# Include path ของโปรเจค + Microchip headers
target_include_directories(${PROJECT_NAME}
    PRIVATE
        include/                    # header โปรเจคนี้ (ev_charger.h, tahoma font.h)
        ${XC16_INCLUDE_DIRS}        # Microchip device headers
)

# -----------------------------------------------------------------------------
# Compiler flags สำหรับ XC16 / PIC24FJ128GA106
# -----------------------------------------------------------------------------
set(MCU_FLAGS "-mcpu=${MCU_NAME}")

target_compile_options(${PROJECT_NAME} PRIVATE
    ${MCU_FLAGS}
    -std=c99          # ใช้ C99 standard
    -O1               # Optimize level 1 (Free license XC16 รองรับ O0 และ O1)
    -Wall             # เปิด warning ทั้งหมด
    -Wextra           # warning เพิ่มเติม
    -ffunction-sections  # แยก function ออกเป็น section ย่อยๆ (ช่วย dead-code elimination)
    -fdata-sections      # แยก data เช่นกัน
    -g                   # เก็บข้อมูล debug (สำหรับ MPLAB debugger)
)

# Preprocessor definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    MCU_VARIANT=1      # เลือก PIC24FJ128GA106 (ดู ev_charger.h)
)

# -----------------------------------------------------------------------------
# Linker flags
# -----------------------------------------------------------------------------
target_link_options(${PROJECT_NAME} PRIVATE
    ${MCU_FLAGS}
    -Wl,--script=${LINKER_SCRIPT}        # ใช้ linker script ของ MCU นี้
    -Wl,--heap=512                       # จอง heap 512 byte (สำหรับ printf)
    -Wl,--stack=512                      # stack 512 byte
    -Wl,--report-mem                     # แสดงขนาด Flash/RAM หลัง link
    -Wl,--gc-sections                    # ลบ section ที่ไม่ถูกใช้ออก
)

# -----------------------------------------------------------------------------
# Post-build: สร้างไฟล์ .hex สำหรับ programming
# -----------------------------------------------------------------------------
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${XC16_PATH}/xc16-bin2hex
        "$<TARGET_FILE:${PROJECT_NAME}>"
    COMMENT "กำลังสร้าง HEX file..."
    VERBATIM
)

# แสดงขนาด Flash/RAM หลัง build สำเร็จ
if(XC16_SIZE)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${XC16_SIZE} --format=berkeley "$<TARGET_FILE:${PROJECT_NAME}>"
        COMMENT "ขนาดไฟล์:"
        VERBATIM
    )
else()
    message(WARNING "ไม่พบ xc16-size/size – ข้ามขั้นตอนแสดงขนาดไฟล์หลัง build")
endif()

# -----------------------------------------------------------------------------
# แสดงข้อมูลสรุป
# -----------------------------------------------------------------------------
message(STATUS "==============================================")
message(STATUS "โปรเจค   : ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "MCU      : PIC${MCU_NAME}")
message(STATUS "Compiler : ${CMAKE_C_COMPILER}")
message(STATUS "==============================================")
