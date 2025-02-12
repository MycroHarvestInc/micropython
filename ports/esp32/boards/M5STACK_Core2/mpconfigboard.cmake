# SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT

# core2 https://github.com/m5stack/m5stack-board-id/blob/main/board.csv#L4
set(BOARD_ID 2)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    ${SDKCONFIG_IDF_VERSION_SPECIFIC}
    boards/sdkconfig.flash_16mb_ota
    boards/sdkconfig.ble
    boards/sdkconfig.240mhz
    boards/sdkconfig.disable_iram
    boards/sdkconfig.spiram
    boards/sdkconfig.freertos
    boards/M5STACK_Core2/sdkconfig.board
)

# LCD configuration
set(LV_CFLAGS -DLV_COLOR_DEPTH=16 -DLV_COLOR_16_SWAP=0)

# Use the board's manifest.py
set(MICROPY_FROZEN_MANIFEST ${MICROPY_BOARD_DIR}/manifest.py)
