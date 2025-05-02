TARGET_BOARDS := radientum_wp_v1_0  radientum_wp_v1_0_events_via_gpio  radientum_wp_v1_0_powerfail_via_gpio radientum_wp_v1_1  radientum_wp_v1_1_events_via_gpio radientum_wp_v1_1_powerfail_via_gpio

INI_FILE_APP=$(APP_SRCS_PATH)dlms_app.ini

KEY_FILE=$(APP_SRCS_PATH)bootloader_keys.ini

#
# App specific configuration
#

# app_specific_area_id is board and meter specific
#

app_specific_area_base ?= 0x8CEC

ifeq ($(target_board), radientum_wp_v1_0)
app_specific_area_board_id=2
else ifeq ($(target_board), radientum_wp_v1_1)
# Same as radientum_wp_v1_0 as only difference is DCDC that is handled in bootloader
app_specific_area_board_id=2
else ifeq ($(target_board), radientum_wp_v1_0_events_via_gpio)
app_specific_area_board_id=3
else ifeq ($(target_board), radientum_wp_v1_1_events_via_gpio)
# Same as radientum_wp_v1_0_events_via_gpio as only difference is DCDC that is handled in bootloader
app_specific_area_board_id=3
else ifeq ($(target_board), radientum_wp_v1_0_powerfail_via_gpio)
app_specific_area_board_id=4
else ifeq ($(target_board), radientum_wp_v1_1_powerfail_via_gpio)
app_specific_area_board_id=4
else
 $(error Board $(target_board) has no app_specific_area_board_id)
endif

# meter_type is not required as settings are provisionned
meter_type_id ?= 0

app_specific_area_id=$(app_specific_area_base)$(app_specific_area_board_id)$(meter_type_id)
$(info app_specific_area_id = $(app_specific_area_id))

# App version
app_major=$(shell printf "%d" 0x`echo $(SHORT_SHA1) | cut -c1-2`)
app_minor=$(shell printf "%d" 0x`echo $(SHORT_SHA1) | cut -c3-4`)
app_maintenance=$(shell printf "%d" 0x`echo $(SHORT_SHA1) | cut -c5-6`)
app_development=$(shell printf "%d" 0x`echo $(SHORT_SHA1) | cut -c7-8`)

# Malloc debug
safe_malloc ?= min
