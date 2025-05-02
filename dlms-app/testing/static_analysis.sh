# Get the list of modified .c and .h files between master and the branch
git fetch origin
echo $BITBUCKET_BRANCH
MODIFIED_FILES=$(git diff --name-only $BITBUCKET_BRANCH origin/master  -- . ':!DLMS_LIB' ':!SDK' ':!board' ':!testing' ':!test_router_app' ':!test_app' ':!dlms_app/preamble_app/hdlc/yahdlc/' | grep '\.c$')
TOOL_VERSION=$(cppcheck --version)

# Check if there are any modified files
if [ -z "$MODIFIED_FILES" ]; then
    echo "No modified .c files to analyze."
else
    echo "Running $TOOL_VERSION on the following files:"
    echo "$MODIFIED_FILES"

    # Run cppcheck on the list of modified files
    # Remember to update MODIFIED_FILES command when changing included folders.
    cppcheck --enable=all --error-exitcode=1 --inline-suppr --platform=testing/arm32-wchar_t2.xml \
                  --suppress=missingIncludeSystem --suppress=unusedFunction \
                  --suppress='*:SDK/mcu/*' --suppress=unusedStructMember \
                  --suppress=unmatchedSuppression \
                  --language=c --std=c99 \
                  -i DLMS_LIB -i SDK -i board -i testing -i test_router_app -i test_app -i dlms_app/preamble_app/hdlc/yahdlc/ \
                  -I board/radientum_wp_v1_0/ -I dlms_app/ -I dlms_app/debug/ -I dlms_app/dlms/ \
                  -I dlms_app/dlms/internal/ -I dlms_app/dlms/profiles/ -I dlms_app/preamble_app/ \
                  -I dlms_app/preamble_app/hdlc/ -I dlms_app/preamble_app/hdlc/yahdlc/C/ \
                  -I dlms_app/preamble_app/provisioning_data/ -I dlms_app/safemalloc/ -I dlms_app/storage/ \
                  -I dlms_app/wirepas/ -I DLMS_LIB/development/ -I DLMS_LIB/development/include/ \
                  -I SDK/api/ -I SDK/libraries/ -I SDK/libraries/dualmcu/ -I SDK/libraries/app_persistent/ -I SDK/libraries/scheduler/ \
                  -I SDK/libraries/shared_appconfig/ -I SDK/libraries/shared_data/ -I SDK/libraries/stack_state/ \
                  -I SDK/mcu/ -I SDK/mcu/common/ -I SDK/mcu/common/cmsis/ -I SDK/mcu/efr/ -I SDK/mcu/efr/common/ \
                  -I SDK/mcu/efr/common/vendor/ -I SDK/mcu/efr/common/vendor/efr32fg23/ \
                  -I SDK/mcu/efr/common/vendor/efr32fg23/Include/ -I SDK/mcu/efr/efr32/ -I SDK/mcu/efr/efr32/hal/ \
                  -I SDK/mcu/efr/efr32/hal/i2c/ -I SDK/mcu/efr/efr32/hal/radio/ -I SDK/mcu/efr/efr32/hal/usart/ \
                  -I SDK/mcu/efr/efr32/hal/usart/series2/ -I SDK/mcu/hal_api/ -I SDK/mcu/hal_api/button/ \
                  -I SDK/mcu/hal_api/gpio/ -I SDK/mcu/hal_api/led/ -I SDK/util/ -I SDK/util/tinyaes/ \
                  -I SDK/util/tinycbor/src/ \
                  -D__GNUC__ -DEFR32_PLATFORM -DEFR32FG23 -DEFR32FG23B020F512IM48 \
                  -DARM_MATH_ARMV8MML -DTARGET_BOARD=radientum_wp_v1_0_events_via_gpio \
                  -DMCU=efr32 -DMCU_SUB=xg23 -DEFR32 -DLOG_DUMP -DLOG_RX_DUMP -DLOG_TX_DUMP \
                  -DNETWORK_ADDRESS=0x6B2A78 -DNETWORK_CHANNEL=1 \
                  -DNET_CIPHER_KEY=0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33 \
                  -DNET_AUTHEN_KEY=0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33 \
                  -DVER_MAJOR=180 -DVER_MINOR=120 -DVER_MAINT=150 -DVER_DEV=246 \
                  -DBOARD_HW_CRYSTAL_32K=0 -DBOARD_HW_DCDC=1 -DBOARD_HW_HFXO_CTUNE=106 \
                  -DBOARD_HW_LFXO_CTUNE=63 -DBOARD_HW_LFXO_GAIN=2 \
                  -DPROVISIONING_UART_BAUDRATE=115200 -DHEAP_FROM_LINKER \
                  -DAUTOMATIC_NODE_ADDRESSING \
                  -DNIC_KEY_ENCRYPTION_KEY=0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33 \
                  -DNIC_AUTHENTICATION_KEY=0x41,0x65,0x4d,0x6c,0x45,0x6b,0x41,0x6b,0x67,0x61,0x50,0x6c,0x30,0x31,0x61,0x62 \
                  -DNIC_ENCRYPTION_KEY=0x41,0x65,0x4d,0x6c,0x45,0x6b,0x41,0x6b,0x67,0x61,0x50,0x6c,0x30,0x31,0x61,0x62 \
                  -DNIC_MR_PASSWORD="1A2B3C4D" -DNIC_US_PASSWORD="AeMlHlSugaPl01ab" \
                  -DNIC_FU_PASSWORD="wp_fu_pass" -DNIC_FLAG_ID="WGE" \
                  -DNIC_BAUDRATE=9600 -DNIC_INTERFACE_TYPE=DLMS_INTERFACE_TYPE_HDLC \
                  -DDLMS_IGNORE_ACCOUNT -DDLMS_IGNORE_ACTION_SCHEDULE -DDLMS_IGNORE_ACTIVITY_CALENDAR \
                  -DDLMS_IGNORE_ARBITRATOR -DDLMS_IGNORE_ARRAY_MANAGER -DDLMS_IGNORE_ASSOCIATION_SHORT_NAME \
                  -DDLMS_IGNORE_AUTO_ANSWER -DDLMS_IGNORE_AUTO_CONNECT -DDLMS_IGNORE_CHARGE -DDLMS_IGNORE_CREDIT \
                  -DDLMS_IGNORE_DEMAND_REGISTER -DDLMS_IGNORE_DISCONNECT_CONTROL -DDLMS_IGNORE_EVENT \
                  -DDLMS_IGNORE_EXTENDED_REGISTER -DDLMS_IGNORE_FLOAT32 -DDLMS_IGNORE_FLOAT64 \
                  -DDLMS_IGNORE_FUNCTION_CONTROL -DDLMS_IGNORE_G3_PLC_6LO_WPAN -DDLMS_IGNORE_G3_PLC_MAC_LAYER_COUNTERS \
                  -DDLMS_IGNORE_G3_PLC_MAC_SETUP -DDLMS_IGNORE_GPRS_SETUP -DDLMS_IGNORE_GSM_DIAGNOSTIC \
                  -DDLMS_IGNORE_HIGH_MD5 -DDLMS_IGNORE_HIGH_SHA1 -DDLMS_IGNORE_HIGH_SHA256 \
                  -DDLMS_IGNORE_IEC_8802_LLC_TYPE1_SETUP -DDLMS_IGNORE_IEC_8802_LLC_TYPE2_SETUP \
                  -DDLMS_IGNORE_IEC_8802_LLC_TYPE3_SETUP -DDLMS_IGNORE_IEC_LOCAL_PORT_SETUP \
                  -DDLMS_IGNORE_IEC_TWISTED_PAIR_SETUP -DDLMS_IGNORE_IP4_SETUP -DDLMS_IGNORE_IP6_SETUP \
                  -DDLMS_IGNORE_LIMITER -DDLMS_IGNORE_LLC_SSCS_SETUP -DDLMS_IGNORE_MAC_ADDRESS_SETUP \
                  -DDLMS_IGNORE_MBUS_CLIENT -DDLMS_IGNORE_MBUS_DIAGNOSTIC -DDLMS_IGNORE_MBUS_MASTER_PORT_SETUP \
                  -DDLMS_IGNORE_MBUS_PORT_SETUP -DDLMS_IGNORE_MBUS_SLAVE_PORT_SETUP -DDLMS_IGNORE_MODEM_CONFIGURATION \
                  -DDLMS_IGNORE_PARAMETER_MONITOR -DDLMS_IGNORE_PLC -DDLMS_IGNORE_PPP_SETUP \
                  -DDLMS_IGNORE_PRIME_NB_OFDM_PLC_APPLICATIONS_IDENTIFICATION \
                  -DDLMS_IGNORE_PRIME_NB_OFDM_PLC_MAC_COUNTERS \
                  -DDLMS_IGNORE_PRIME_NB_OFDM_PLC_MAC_FUNCTIONAL_PARAMETERS \
                  -DDLMS_IGNORE_PRIME_NB_OFDM_PLC_MAC_NETWORK_ADMINISTRATION_DATA \
                  -DDLMS_IGNORE_PRIME_NB_OFDM_PLC_MAC_SETUP -DDLMS_IGNORE_PRIME_NB_OFDM_PLC_PHYSICAL_LAYER_COUNTERS \
                  -DDLMS_IGNORE_REGISTER -DDLMS_IGNORE_REGISTER_ACTIVATION -DDLMS_IGNORE_REGISTER_MONITOR \
                  -DDLMS_IGNORE_REGISTER_TABLE -DDLMS_IGNORE_SAP_ASSIGNMENT -DDLMS_IGNORE_SCHEDULE \
                  -DDLMS_IGNORE_SCRIPT_TABLE -DDLMS_IGNORE_SFSK_ACTIVE_INITIATOR -DDLMS_IGNORE_SFSK_MAC_COUNTERS \
                  -DDLMS_IGNORE_SFSK_MAC_SYNCHRONIZATION_TIMEOUTS -DDLMS_IGNORE_SFSK_PHY_MAC_SETUP \
                  -DDLMS_IGNORE_SFSK_REPORTING_SYSTEM_LIST -DDLMS_IGNORE_SMTP_SETUP -DDLMS_IGNORE_SPECIAL_DAYS_TABLE \
                  -DDLMS_IGNORE_STATUS_MAPPING -DDLMS_IGNORE_TCP_UDP_SETUP -DDLMS_IGNORE_TOKEN_GATEWAY \
                  -DDLMS_IGNORE_UTILITY_TABLES -DDLMS_IGNORE_WIRELESS_MBUS -DDLMS_IGNORE_WIRELESS_MODE_Q_CHANNEL \
                  -DDLMS_IGNORE_ZIG_BEE_NETWORK_CONTROL -DDLMS_IGNORE_ZIG_BEE_SAS_APS_FRAGMENTATION \
                  -DDLMS_IGNORE_ZIG_BEE_SAS_JOIN -DDLMS_IGNORE_ZIG_BEE_SAS_STARTUP \
                  -DDLMS_USE_CUSTOM_MALLOC -DDLMS_USE_EPOCH_TIME -DDLMS_USE_UTC_TIME_ZONE \
                  -DSFMALLOC_SAVE_SPACE -DUSE_SAFE_MALLOC -Dgxfree=sf_free -Dgxmalloc=sf_malloc \
                  -Dgxcalloc=sf_calloc -Dgxrealloc=sf_realloc -DDEBUG \
                  $MODIFIED_FILES -j $(nproc)

fi
