TARGET_BOARDS := radientum_wp_v1_0  radientum_wp_v1_0_events_via_gpio  radientum_wp_v1_0_sink_app radientum_wp_v1_1  radientum_wp_v1_1_events_via_gpio radientum_wp_v1_1_sink_app

INI_FILE_APP=$(APP_SRCS_PATH)test_app.ini
KEY_FILE=$(APP_SRCS_PATH)bootloader_keys.ini

#
# No network default settings
#

#
# App specific configuration
#

# Define a specific application area_id
app_specific_area_id=0x657866

# App version
app_major=1
app_minor=2
app_maintenance=0
app_development=0
