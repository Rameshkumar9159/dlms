#!/usr/bin/env python3

# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging
import sys

from importlib.metadata import version
from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import AssociationLevelEnum, DLMSNetworkInterface, ErrorCodeEnum, MeterConfiguration, Meter


# TODO: update the node informations.
NODE_ID: int = <node_id>
GATEWAY_ID: str = <dlms_test_network>
SINK_ID: str = <sink_id>
NIC_SYSTEM_TITLE_FLAG: str = <nic_manufacturer_id>
INVOKE_ID: int = 0
INVOCATION_COUNTER: int = 0

# TODO: Complete your MQTT settings to connect to the MQTT broker.
MQTT_HOST: str = <mqtt_host>
MQTT_PORT: int = <mqtt_port>
MQTT_USERNAME: str = <mqtt_username>
MQTT_PASSWORD: str = <mqtt_password>

# Network id to filter the DLMS messages. MQTT client will drop the messages that are not from this network.
# If it is set to None it means that no filtering on network id will be done.
NETWORK_ADDRESS: int = None

# TODO: Meter configuration of the meter to query.
# Note: If some keys are not needed, they can be removed to the configuration or left to None.
METER_CONFIGURATION = MeterConfiguration(
    key_encryption_key="<key_encryption_key_in_hex>",
    authentication_key="<authentication_key_in_hex>",
    block_cipher_key="<block_cipher_key_in_hex>",
    mr_password="<mr_password_in_hex>",
    us_password="<us_password_in_hex>",
    fu_password="<fu_password_in_hex>"
)


if __name__ == "__main__":
    # Check python library version
    INTEGRATION_VERSION = "1.4"
    assert version('wirepas_dlms_tool') == INTEGRATION_VERSION, \
        f"Wirepas DLMS tool Python must be version {INTEGRATION_VERSION}, but found {version('wirepas_dlms_tool')}"

    # Set up the logs
    log_filename = "example_request.log"
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
                                  MQTT_PASSWORD, strict_mode=False)

    # Our network interface to communication with meters.
    dni = DLMSNetworkInterface(wni, nodes=[NODE_ID], network=NETWORK_ADDRESS)

    # Creation of a meter object.
    system_title = Meter.generate_system_title(NIC_SYSTEM_TITLE_FLAG, NODE_ID)
    meter = dni.create_meter(node_id=NODE_ID,
                             meter_configuration=METER_CONFIGURATION,
                             nic_system_title=system_title,
                             gateway=GATEWAY_ID,
                             sink=SINK_ID,
                             response_timeout_s=30)

    # Set the IC and invoke id of the meter object.
    meter.set_invocation_counter(INVOCATION_COUNTER)
    meter.set_invoke_id(INVOKE_ID)

    # The code below can be executed to query the NIC server for its invocation counter.
    # print("Get the invocation counter of the NIC in PC before any request")
    # if meter.establish_AA_NIC(connection_type=AssociationLevelEnum.PC_ASSOCIATION):
    #     meter.get_NIC_invocation_counter()
    #     meter.release_AA_NIC()

    # Get meter response of a get device ID request in pass-through.
    response = meter.get_meter_device_ID(AssociationLevelEnum.US_ASSOCIATION)

    # Verification of the correctness of the response with the error code.
    assert response.error_code == ErrorCodeEnum.RES_OK, "No valid response was received!"

    # We can print the device ID from the value of the payload that has been returned by the meter.
    logging.info(f"The device ID of the meter is {response.value}")