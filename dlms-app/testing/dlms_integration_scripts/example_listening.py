#!/usr/bin/env python3

# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging

from importlib.metadata import version
from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import AssociationLevelEnum, DLMSNetworkInterface, MeterConfiguration, WirepasNotification


# TODO: Complete your MQTT settings to connect to the MQTT broker.
MQTT_HOST: str = <mqtt_host>
MQTT_PORT: int = <mqtt_port>
MQTT_USERNAME: str = <mqtt_username>
MQTT_PASSWORD: str = <mqtt_password>

# Network id to filter the DLMS messages. MQTT client will drop the messages that are not from this network.
# If it is set to None it means that no filtering on network id will be done.
NETWORK_ADDRESS: int = None

# TODO: List all the meter configurations of the meters you are listening to then add the meters that need to be listened to.
# Note: If some keys are not needed, they can be removed to the configuration or left to None.
meter_configurations: dict = {
    "Meter configuration 1": MeterConfiguration(
        key_encryption_key="<key_encryption_key_in_hex>",
        authentication_key="<authentication_key_in_hex>",
        block_cipher_key="<block_cipher_key_in_hex>",
        mr_password="<mr_password_in_hex>",
        us_password="<us_password_in_hex>",
        fu_password="<fu_password_in_hex>"
    ),
    "Meter configuration 2": MeterConfiguration(
        key_encryption_key="<key_encryption_key_in_hex>",
        authentication_key="<authentication_key_in_hex>",
        block_cipher_key="<block_cipher_key_in_hex>",
        mr_password="<mr_password_in_hex>",
        us_password="<us_password_in_hex>",
        fu_password="<fu_password_in_hex>"
    )
}

# TODO: Map the serial number in bytes of the meter to provision with their associated configuration.
meters_to_listen = {
    <node id in int>: meter_configurations["Meter configuration 1"],
    <a second node id in int>: meter_configurations["Meter configuration 1"],
    <another node id in int>: meter_configurations["Meter configuration 2"]
}


def default_notification_cb(meter, notification):
    """ Function to be called when receiving a notification from meters. """
    print(f"A notification has been received from {meter.node_id}:")
    print(notification.xml)


def unparsed_cb(meter, payload: bytes):
    """ Callback to be called when a message could not be parsed. """
    print(f"An unknown DLMS message has been received from {meter.node_id} in {meter.gateway_id}/{meter.sink_id}.")


if __name__ == "__main__":
    # Check python library version
    INTEGRATION_VERSION = "1.4"
    assert version('wirepas_dlms_tool') == INTEGRATION_VERSION, \
        f"Wirepas DLMS tool Python must be version {INTEGRATION_VERSION}, but found {version('wirepas_dlms_tool')}"

    # Set up the logs.
    log_filename = "example_listening.log"
    print(f"Additonal information on the DLMS exchanges can be found in the following script logs: {log_filename}")

    logging.basicConfig(
        filename=log_filename,
        format='%(asctime)s | [%(levelname)s] %(filename)s:%(lineno)d:%(funcName)s:%(message)s',
        level="DEBUG")

    # Use for connection to the MQTT.
    wni = WirepasNetworkInterface(MQTT_HOST, MQTT_PORT, MQTT_USERNAME,
                                  MQTT_PASSWORD, strict_mode=False)

    # Our network interface to communication with meters.
    dni = DLMSNetworkInterface(wni, default_NIC_status_cb=default_notification_cb,
                               default_notification_cb=default_notification_cb,
                               default_unparsed_cb=unparsed_cb,
                               network=NETWORK_ADDRESS)

    # Create the meter objects for the meter that need to be listened to.
    for node_id, meter_configuration in meters_to_listen.items():
        dni.create_meter(node_id=node_id,
                         meter_configuration=meter_configuration,
                         NIC_status_cb=default_notification_cb,
                         notification_cb=default_notification_cb,
                         unparsed_cb=unparsed_cb)

    # Make the script run.
    input("Press 'Enter' keyword to stop the script!\n")