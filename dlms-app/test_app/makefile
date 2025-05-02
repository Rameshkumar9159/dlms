DEBUG_FOLDER := $(SRCS_PATH)../dlms_app/debug/

# Prepend our include path to be able to replace problematic SDK files w/o patching the SDK
INCLUDES := -I$(DEBUG_FOLDER) $(INCLUDES)
INCLUDES += -I$(SRCS_PATH)

# Use App Scheduler and configure it
APP_SCHEDULER=yes
APP_SCHEDULER_TASKS=8

# This application use HAL for leds
HAL_LED=yes

SHARED_DATA=yes

INCLUDES += -Ibootloader_test/timing
SRCS += bootloader_test/timing/timing.c

SRCS += $(SRCS_PATH)dut_protocol.c
SRCS += $(SRCS_PATH)extflash_tests.c
SRCS += $(SRCS_PATH)factory_rf_tests_dut.c

HAL_UART=yes
HAL_GPIO = yes

PREAMBLE_APP_FOLDER := $(SRCS_PATH)../dlms_app/preamble_app/
HDLC_FOLDER := $(PREAMBLE_APP_FOLDER)hdlc/

SRCS += $(HDLC_FOLDER)yahdlc/C/fcs.c
SRCS += $(HDLC_FOLDER)yahdlc/C/yahdlc.c

SRCS += $(HDLC_FOLDER)hdlc.c

INCLUDES += -I$(HDLC_FOLDER)
INCLUDES += -I$(HDLC_FOLDER)yahdlc/C/

# PROVISIONING_UART_BAUDRATE in use for Provisioning
CFLAGS += -DPROVISIONING_UART_BAUDRATE=115200

# Enable application debug prints
CFLAGS += -DDEBUG_LOG_PRINT_TIME_MS
APP_PRINTING=custom
CFLAGS += -DDEBUG_LOG_UART_BAUDRATE=460800

ifneq ($(APP_PRINTING), no)
SRCS += $(DEBUG_FOLDER)uart_print.c
endif
