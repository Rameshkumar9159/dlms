# Dual MCU app needs UART
HAL_UART=yes
ifeq ($(uart_use_usb),yes)
UART_USE_DMA=no
UART_USE_USB=yes
CFLAGS += -DUART_USE_USB
else ifeq ($(uart_use_dma),no)
UART_USE_DMA=no
UART_USE_USB=no
else
# dma uart is used by default if not explicitly set to no
UART_USE_DMA=yes
UART_USE_USB=no
endif

# Tells if the gateway associated to the board supports only one sink
ifeq ($(mono_sink_gateway),yes)
CFLAGS += -DMONO_SINK_GATEWAY
endif

# Define default baudrate and hardware flow control settings
uart_br ?= 125000
uart_fc ?= false

CFLAGS += -DUART_BAUDRATE=$(uart_br)
CFLAGS += -DUART_FLOWCONTROL=$(uart_fc)

DUALMCU_LIB=yes

STACK_STATE_LIB=yes
STACK_STATE_CBS=1
