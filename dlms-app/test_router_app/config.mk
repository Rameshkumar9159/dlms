TARGET_BOARDS := radientum_wp_v1_0  radientum_wp_v1_0_events_via_gpio  radientum_wp_v1_0_sink_app radientum_wp_v1_1  radientum_wp_v1_1_events_via_gpio radientum_wp_v1_1_sink_app

KEY_FILE=$(APP_SRCS_PATH)bootloader_keys.ini

#
# No network default settings
#

#
# App specific configuration
#

# Define a specific application area_id
app_specific_area_id=0x826f75

# App version
app_major=1
app_minor=0
app_maintenance=0
app_development=0
