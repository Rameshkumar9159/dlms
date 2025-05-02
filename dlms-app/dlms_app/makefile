DLMS_PATH := $(SRCS_PATH)../DLMS_LIB/development/
DLMS_PREFIX := $(DLMS_PATH)src/

# DLMS_APP flags
#CFLAGS += -DDLMS_APP_NO_LED

#CFLAGS += -DDLMS_DEBUG #This is actually for server debug messages

include $(SRCS_PATH)preamble_app/makefile
include $(SRCS_PATH)gurux.defines
CFLAGS += $(GURUX_FLAGS)


# You can add more sources here if needed
SRCS +=  $(SRCS_PATH)preamble_app/preamble_app.c \
         $(SRCS_PATH)dlms/opt_passthrough.c \
         $(SRCS_PATH)common.c \
         $(SRCS_PATH)led_manager.c \
         $(SRCS_PATH)wirepas/wirepas_com.c \
         $(SRCS_PATH)wirepas/wirepas_meter_firmware_update.c \
         $(SRCS_PATH)wirepas/wirepas_ping.c \
         $(SRCS_PATH)dlms/meter_connection_management.c \
         $(SRCS_PATH)dlms/profiles/instantaneous_profile.c \
         $(SRCS_PATH)dlms/profiles/blockload_profile.c \
         $(SRCS_PATH)dlms/profiles/dailyload_profile.c \
         $(SRCS_PATH)dlms/profiles/name_plate_profile.c \
         $(SRCS_PATH)dlms/profiles/billing_profile.c \
         $(SRCS_PATH)dlms/profiles/event_log_profiles.c \
         $(SRCS_PATH)dlms/profiles/fixed_day_billing_profile.c \
         $(SRCS_PATH)dlms/profiles/export_billing_profile.c \
         $(SRCS_PATH)dlms/internal/dlms_com.c \
         $(SRCS_PATH)dlms/internal/meter_uart.c \
         $(SRCS_PATH)dlms/internal/application_association.c \
         $(SRCS_PATH)dlms/internal/profile_generic.c \
         $(SRCS_PATH)dlms/dlms_lock.c \
         $(SRCS_PATH)dlms/dlms_id.c \
         $(SRCS_PATH)dlms/nic_server.c \
         $(SRCS_PATH)dlms/nic_server_objects.c \
         $(SRCS_PATH)dlms/nic_status.c \
         $(SRCS_PATH)dlms/read_esw.c \
         $(SRCS_PATH)dlms/rng.c \
         $(SRCS_PATH)dlms/meter_clock.c \
         $(SRCS_PATH)dlms/security_material.c \
         $(SRCS_PATH)dlms/internal/data_notification.c \
         $(SRCS_PATH)dlms/security_service.c \
         $(SRCS_PATH)dlms/nic_system_title.c \
         $(SRCS_PATH)dlms/firmware_update.c \
         $(SRCS_PATH)dlms/transparent_mode.c \
         $(SRCS_PATH)storage/storage_driver.c \
         $(SRCS_PATH)storage/client_attribute_manager.c \
         $(SRCS_PATH)storage/server_attribute_manager.c \
         $(SRCS_PATH)wirepas/wirepas_eol_testing.c \
         $(SRCS_PATH)safemalloc/sfmalloc.c \


SRCS += $(custom_srcs)

# Prepend our include path to be able to replace problematic SDK files w/o patching the SDK
INCLUDES := -I$(SRCS_PATH)debug $(INCLUDES)

INCLUDES += -I$(DLMS_PATH)
INCLUDES += -I$(SRCS_PATH)
INCLUDES += -I$(SRCS_PATH)dlms/
INCLUDES += -I$(SRCS_PATH)dlms/internal/
INCLUDES += -I$(SRCS_PATH)dlms/profiles/
INCLUDES += -I$(SRCS_PATH)wirepas/
INCLUDES += -I$(SRCS_PATH)storage/
INCLUDES += -I$(SRCS_PATH)safemalloc/

CFLAGS += -DHEAP_FROM_LINKER
CFLAGS += -Werror
CFLAGS += -fms-extensions # Allow extended struct

# Define default network settings
CFLAGS += -DNETWORK_ADDRESS=$(default_network_address)
CFLAGS += -DNETWORK_CHANNEL=$(default_network_channel)

ifeq ($(automatic_node_addressing), yes)
CFLAGS += -DAUTOMATIC_NODE_ADDRESSING
$(info Beware: Automatic Node Addressing is enabled! Please do not use it for a production build!)
endif

# Define a function to check if a string is alphabetic
is_string_3_alphabetic_chars = $(shell echo "$(1)" | grep -Eq '^[[:alpha:]]{3}$$' && echo true || echo false)

# Define default NIC settings
ifneq ($(default_nic_key_encryption_key),)
CFLAGS += -DNIC_KEY_ENCRYPTION_KEY=$(default_nic_key_encryption_key)
endif
ifneq ($(default_nic_authentication_key),)
CFLAGS += -DNIC_AUTHENTICATION_KEY=$(default_nic_authentication_key)
endif
ifneq ($(default_nic_encryption_key),)
CFLAGS += -DNIC_ENCRYPTION_KEY=$(default_nic_encryption_key)
endif
ifneq ($(default_nic_mr_password),)
CFLAGS += -DNIC_MR_PASSWORD=\"$(default_nic_mr_password)\"
endif
ifneq ($(default_nic_us_password),)
CFLAGS += -DNIC_US_PASSWORD=\"$(default_nic_us_password)\"
endif
ifneq ($(default_nic_fu_password),)
CFLAGS += -DNIC_FU_PASSWORD=\"$(default_nic_fu_password)\"
endif
ifneq ($(default_nic_flag_id),)
 ifeq ($(call is_string_3_alphabetic_chars,$(default_nic_flag_id)),true)
 CFLAGS += -DNIC_FLAG_ID=\"$(default_nic_flag_id)\"
 else
 $(error NIC Flag ID must be specified as 3 ISO Alphabetic characters)
 endif
endif
ifneq ($(default_nic_baudrate),)
CFLAGS += -DNIC_BAUDRATE=$(default_nic_baudrate)
endif
ifeq ($(default_nic_interface_type),HDLC)
CFLAGS += -DNIC_INTERFACE_TYPE=DLMS_INTERFACE_TYPE_HDLC
else ifeq ($(default_nic_interface_type),WRAPPER)
CFLAGS += -DNIC_INTERFACE_TYPE=DLMS_INTERFACE_TYPE_WRAPPER
else
CFLAGS += -DNIC_INTERFACE_TYPE=UNDEFINED_DLMS_INTERFACE_TYPE
endif


# Use App Scheduler and configure it
# List of tasks
# =============
# git grep -E "App_Scheduler_addTask_execTime|RESCHEDULE" | grep -v -E "(#~|RESCHEDULE_WITH_DELAY|\(\)|\(task)" | cut -d'(' -f2 | cut -d',' -f1 | cut -d')' -f1 | sort -u | tee >(wc -l)
#~ _lock_release
#~ close_connection_fsm
#~ establish_aa_fsm
#~ firmwareUpdateHandler
#~ open_connection_fsm
#~ profile_generic_read_fsm
#~ read_billing_profile_fsm
#~ read_block_load_profile_fsm
#~ read_daily_load_profile_fsm
#~ read_data_block_fsm
#~ read_dlms_packet_fsm
#~ read_esw_fsm
#~ read_event_log_fsm
#~ read_event_logs_task
#~ read_instantaneous_profile_fsm
#~ read_meter_name_fsm
#~ read_name_plate_profile_fsm
#~ release_aa_fsm
#~ run_passthrough_fsm
#~ serverHandler
#~ update_leds_task
#~ update_meter_led_task
#~ update_security_material_fsm
#~eol_test_send_result_task
#~eol_test_timeout_task
#~meter_com_ok_notification_task
#~ 26

APP_SCHEDULER=yes
APP_SCHEDULER_TASKS=26

SHARED_DATA=yes

# Enabled Stack state library
STACK_STATE_LIB=yes
STACK_STATE_CBS=2

# Enable application debug prints
CFLAGS += -DDEBUG_LOG_PRINT_TIME_MS
APP_PRINTING=custom
CFLAGS += -DDEBUG_LOG_UART_BAUDRATE=460800

HAL_UART=yes
HAL_GPIO = yes
HAL_LED=yes

ifneq ($(APP_PRINTING), no)
SRCS += $(SRCS_PATH)debug/uart_print.c
SRCS += $(SRCS_PATH)debug/trace_boot.c
endif

# Export app version as C flag
CFLAGS += -DAPP_MAJOR=$(app_major)
CFLAGS += -DAPP_MINOR=$(app_minor)
CFLAGS += -DAPP_MAINT=$(app_maintenance)
CFLAGS += -DAPP_DEV=$(app_development)

# Sanity check, will avoid a makefile recursion
ifeq ($(UTIL_PATH),)
$(error You must build from the root directory)
endif
# Generic util functions are needed (api.c)
include $(UTIL_PATH)makefile

##########################
# DLMS APP CUSTOMIZATION #
##########################
ifneq ($(nic_rx_buffer_size),)
$(info Setting rx buffer to $(nic_rx_buffer_size))
CFLAGS += -DDLMS_COM_PDU_SIZE=$(nic_rx_buffer_size)
endif

ifneq ($(esw_polling_period),)
$(info Setting ESW polling period to $(esw_polling_period) seconds)
CFLAGS += -DESW_POLLING_PERIOD_S=$(esw_polling_period)
endif

ifeq ($(meter_registration_enabled),yes)
CFLAGS += -DMETER_REGISTRATION_ENABLED
endif

ifeq ($(fixed_day_billing_enabled),yes)
CFLAGS += -DFIXED_DAY_BILLING_ENABLED
endif

ifeq ($(use_protected_release),yes)
CFLAGS += -DUSE_PROTECTED_RELEASE=true
else
CFLAGS += -DUSE_PROTECTED_RELEASE=false
endif

ifeq ($(meter_retrofit),yes)
CFLAGS += -DMETER_RETROFIT
endif

ifneq ($(meter_retrofit_flag_id),)
 ifeq ($(call is_string_3_alphabetic_chars,$(meter_retrofit_flag_id)),true)
 CFLAGS += -DMETER_RETROFIT_FLAG_ID=\"$(meter_retrofit_flag_id)\"
 else
 $(error Meter Retrofit Flag ID must be specified as 3 ISO Alphabetic characters)
 endif

endif

# Override link script to increase APP FLASH SIZE
LDSCRIPT = $(SRCS_PATH)gcc_app_efr32xg23xxxxf512.ld

# Directory containing the libraries
GURUX_LIBDIR = $(DLMS_PATH)lib

# Absolute path to the static library
GURUX_LIBS = $(GURUX_LIBDIR)/libgurux_dlms_c.a

# Add other libraries as needed
LIBS += $(GURUX_LIBS)

$(GURUX_LIBS):
	$(MAKE) -f $(SRCS_PATH)gurux.makefile RELATIVE_PATH=$(app_extra_folder)/
