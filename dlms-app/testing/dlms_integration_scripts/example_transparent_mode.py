#!/usr/bin/env python3

# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging
import sys

from importlib.metadata import version
from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import AssociationLevelEnum, DLMSNetworkInterface, ErrorCodeEnum, MeterConfiguration
# TODO: Modify this value to True/False to activate/desactive the transparent mode on the NIC.
ENABLE_TRANSPARENT_MODE = True

# TODO: update the node informations.
NODE_ID: int = 4262104928
GATEWAY_ID: str = "wirepas-gateway"
SINK_ID: str = "sink1"
NIC_SYSTEM_TITLE: bytes = b'\x71\x77\x65\x72\x74\x79\x75\x69' 

# TODO: Complete your MQTT settings to connect to the MQTT broker.
MQTT_HOST: str = "mqtt.eclipseprojects.io"
MQTT_PORT: int = 1883
MQTT_USERNAME: str = None
MQTT_PASSWORD: str = None
MQTT_FORCE_UNSECURE: str = True

# Network id to filter the DLMS messages. MQTT client will drop the messages that are not from this network.
# If it is set to None it means that no filtering on network id will be done.
NETWORK_ADDRESS: int = 7023224 

# TODO: Meter configuration of the meter to query.
# Note: If some keys are not needed, they can be removed to the configuration or left to None.
METER_CONFIGURATION = MeterConfiguration(
    key_encryption_key="",
    authentication_key="62626262626262626262626262626262",
    block_cipher_key="62626262626262626262626262626262",
    mr_password="313233343536",
    us_password="77777777777777777777777777777777",
    fu_password="77777777777777777777777777777777"
)
#METER_CONFIGURATION = MeterConfiguration(
    #key_encryption_key="32323232323232323232323232323232",      # "2222222222222222"
    #authentication_key="32323232323232323232323232323232",      # "2222222222222222"
    #block_cipher_key=None,
    #mr_password="67656e65736973",                                # "genesis"
    #us_password="314132423343344435463637384748",                # "1A2B3C4D5F678GH"
    #fu_password="314132423343344435463637384748"                 # "1A2B3C4D5F678GH"
#)

if __name__ == "__main__":
    # Check python library version
    INTEGRATION_VERSION = "1.4"
    assert version('wirepas_dlms_tool') == INTEGRATION_VERSION, \
        f"Wirepas DLMS tool Python must be version {INTEGRATION_VERSION}, but found {version('wirepas_dlms_tool')}"

    # Set up the logs
    log_filename = "example_activate_transparent_mode.log"
    print(f"The DLMS exchanges can be found in the following script logs: {log_filename}")

    logging.basicConfig(
        format='%(asctime)s | [%(levelname)s] %(filename)s:%(lineno)d:%(funcName)s:%(message)s',
        level="DEBUG",
        handlers=[
            logging.StreamHandler(sys.stdout),
            logging.FileHandler(log_filename, mode="w")
        ]
    )

    # Prepare the connection to the wirepas network.
    wni = WirepasNetworkInterface(MQTT_HOST, MQTT_PORT, MQTT_USERNAME,
                                  MQTT_PASSWORD,MQTT_FORCE_UNSECURE, strict_mode=False)

    # Our network interface to communication with meters.
    dni = DLMSNetworkInterface(wni, nodes=[NODE_ID], network=NETWORK_ADDRESS)

    # Creation of a meter object.
    meter = dni.create_meter(node_id=NODE_ID,
                             meter_configuration=METER_CONFIGURATION,
                             nic_system_title=NIC_SYSTEM_TITLE,
                             gateway=GATEWAY_ID,
                             sink=SINK_ID,
                             response_timeout_s=30)

    # Get the invocation counter of the NIC in PC before any request
    if meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        meter.get_NIC_invocation_counter()
        meter.release_AA_NIC()


    # Activate the transparent mode on the NIC after establishing the association.
    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        meter.set_NIC_transparent_mode_configuration(ENABLE_TRANSPARENT_MODE)
        meter.release_AA_NIC()
