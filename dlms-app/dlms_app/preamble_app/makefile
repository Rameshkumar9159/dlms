PREAMBLE_APP_FOLDER := $(SRCS_PATH)preamble_app/

PROVISIONING_DATA_FOLDER := $(PREAMBLE_APP_FOLDER)provisioning_data/
HDLC_FOLDER := $(PREAMBLE_APP_FOLDER)hdlc/
DEBUG_FOLDER := $(SRCS_PATH)debug/

# You can add more sources here and libraries if needed
SRCS += $(PROVISIONING_DATA_FOLDER)provisioning_data.c

SRCS += $(HDLC_FOLDER)yahdlc/C/fcs.c
SRCS += $(HDLC_FOLDER)yahdlc/C/yahdlc.c

SRCS += $(HDLC_FOLDER)hdlc.c

SRCS += $(PREAMBLE_APP_FOLDER)nic_provisioning.c


INCLUDES += -I$(PREAMBLE_APP_FOLDER)

INCLUDES += -I$(PROVISIONING_DATA_FOLDER)

INCLUDES += -I$(HDLC_FOLDER)
INCLUDES += -I$(HDLC_FOLDER)yahdlc/C/

# Use App Persistent
APP_PERSISTENT=yes

# PROVISIONING_UART_BAUDRATE in use for Provisioning
CFLAGS += -DPROVISIONING_UART_BAUDRATE=115200

TINY_CBOR=yes
