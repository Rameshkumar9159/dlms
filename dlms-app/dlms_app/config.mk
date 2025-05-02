#
# Network default settings configuration
#

# !WARNING! Uncomment for Development/Internal/Testing use ONLY
# Please DO NOT use this setting in production
# Node address should NOT be automatically generated for
# devices that will be deployed in the field
automatic_node_addressing ?= yes

# !WARNING! This value must be set for the compilation to be successful
# Please note that this value can be later changed through the use of the Provisioning script
#default_network_address ?= 0xC8C431   #THIS WILL BE MODIFIED IN DOWN
default_network_address ?= 0X6B2A78    #THIS IS SAME AS THE SINK NODE
# !WARNING! Please DO NOT change this default value.
# Otherwise, please contact Wirepas beforehand.
default_network_channel ?= 1
# !HIGHLY RECOMMENDED! : Please fill the lines below with a
#                        randomly generated authentication & encryption keys (exactly 16 bytes)
default_network_cipher_key ?= 0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33
default_network_authen_key ?= 0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33

#
# NIC default configuration
#

# To provision security keys please fill the lines below
# with the associated meter keys (exactly 16 bytes)
# Example: 0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33
# In case your key is in ascii, it can be converted to this format with this one-line python
# python3 -c "print('0x' + ',0x'.join(x.encode(\"ascii\").hex() for x in \"3333333333333333\"))"
# Encryption key: 2222222222222222
default_nic_encryption_key	?= 0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32
# Authentication key: 2222222222222222
default_nic_authentication_key 	?= 0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32
# Master key: AAAAAAAAAAAAAAAA
default_nic_key_encryption_key 	?= 0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41,0x41
# To provision passwords please fill the lines below
# with the associated meter passwords (maximum 32 characters)
# Example: password
default_nic_mr_password	?= 123456
default_nic_us_password	?= wwwwwwwwwwwwwwww
default_nic_fu_password	?= wwwwwwwwwwwwwwww
# To provision DLMS Flag ID please fill the line below
# with the assigned DLMS Flag ID (3 alphabetic characters, case insensitive)
# Example : WPS
default_nic_flag_id	?= WPS
# To provision baudrate please fill the line below
# with the meter baudrate
default_nic_baudrate ?= 9600
# To provision interface type please fill the line below
# with the desired interface type
# Example : HDLC
# Example : WRAPPER
default_nic_interface_type ?= WRAPPER

# Include base config
# (Relative to SDK)
include  ../dlms_app/base_config.mk
