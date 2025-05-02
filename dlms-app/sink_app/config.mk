TARGET_BOARDS := radientum_wp_v1_0_sink_app radientum_wp_v1_1_sink_app radientum_wp_v1_0_emb_gw_sink_app radientum_wp_v1_1_emb_gw_sink_app

INI_FILE_APP=$(APP_SRCS_PATH)dualmcu_app.ini

KEY_FILE=$(APP_SRCS_PATH)bootloader_keys.ini

#
# Network default settings configuration
#

# If this section is removed, node has to be configured in
# a different way
default_network_address ?= 0x6B2A78
default_network_channel ?= 1
# !HIGHLY RECOMMENDED! : To enable security keys please un-comment the lines below and fill with a
#                        randomly generated authentication & encryption keys (exactly 16 bytes)
default_network_cipher_key ?= 0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33
default_network_authen_key ?= 0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33

# Define a specific application area_id
app_specific_area_id=0x846B74

# App version
app_major=$(sdk_major)
app_minor=$(sdk_minor)
app_maintenance=$(sdk_maintenance)
app_development=$(sdk_development)

