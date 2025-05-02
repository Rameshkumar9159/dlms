DEBUG_FOLDER := $(SRCS_PATH)../dlms_app/debug/

# Prepend our include path to be able to replace problematic SDK files w/o patching the SDK
INCLUDES := -I$(DEBUG_FOLDER) $(INCLUDES)
INCLUDES += -I$(SRCS_PATH)
INCLUDES += -I$(SRCS_PATH)protocols

eol_mode ?= false

SHARED_DATA=yes
HAL_LED=yes
HAL_UART=yes
APP_SCHEDULER=yes

# Constant tasks:
#   - 2 for HDLC
#   - 2 for test router (mini-beacon sending + LED blink)
#   - 2 to manage 2 request in parallel (very unlikely)
#   - 1 to stop test session   
APP_SCHEDULER_TASKS=7

# Configure test_router_app operating mode
# eol_mode=true > End of Line testing
# any other value (or not set) > Standalone NIC testing
ifeq ($(eol_mode), true)
CFLAGS += -DEOL_MODE
$(info End of Line testing mode enabled.)
endif

SRCS += $(SRCS_PATH)protocol.c

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
