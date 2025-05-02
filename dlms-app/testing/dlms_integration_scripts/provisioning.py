#!/usr/bin/env python3

# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging

from importlib.metadata import version
from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import ConnectionStatusEnum, DLMSNetworkInterface, MeterConfiguration, NicStatusReason, NicStatusWord, Meter


# TODO: Complete your MQTT settings to connect to the MQTT broker.
MQTT_HOST: str = <mqtt_host>
MQTT_PORT: int = <mqtt_port>
MQTT_USERNAME: str = <mqtt_username>
MQTT_PASSWORD: str = <mqtt_password>

# Network id to filter the DLMS messages. MQTT client will drop the messages that are not from this network.
# If it is set to None it means that no filtering on network id will be done.
NETWORK_ADDRESS: int = None

# TODO: Input the meter configuration of the unprovisioned meters.
# Note: If some keys are not needed, they can be removed to the configuration or left to None.
#       This configuration must be aligned with the configuration set in dlms_app when the NIC was flashed.
default_configuration = MeterConfiguration(
    key_encryption_key=b"3333333333333333".hex(),
    authentication_key=b"2222222222222222".hex(),
    block_cipher_key=b"1111111111111111".hex(),
    mr_password=b"wp_mr_pass".hex(),
    us_password=b"wp_us_pass".hex(),
    fu_password=b"wp_fu_pass".hex()
)

# TODO: List all the meter configurations of the meters you are listening to.
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
meters_to_provision = {
    <serial number in bytes>: meter_configurations["Meter configuration 1"],
    <a second serial number in bytes>: meter_configurations["Meter configuration 1"],
    <another serial number in bytes>: meter_configurations["Meter configuration 2"]
}


def provisioning_cb(meter: Meter, notification: NicStatusWord):
    """ Callback to be called when a notification message is received.
    The function provisions the meter that sent a NIC status word notification.
    """
    if notification.PC_connection_status != ConnectionStatusEnum.TESTED_SUCCESSFUL or \
            notification.US_connection_status != ConnectionStatusEnum.TESTED_UNSUCCESSFUL:
        return

    logging.info(notification)
    if notification.serial_number in meters_to_provision:
        logging.info(f"The meter {meter.node_id} has to be provisioned.")
        configuration = meters_to_provision[notification.serial_number]

        # Update the meter objects attributes.
        meter.update_meter(meter_configuration=default_configuration,
                           response_timeout_s=30)

        # Try to set the new keys and register the new meter.
        set_registration_bool = (NicStatusReason.NIC_REGISTRATION in notification.nic_status_reason)
        meter.do_registration(send_keys=True,
                              new_configuration=configuration,
                              set_registration=set_registration_bool)



if __name__ == "__main__":
    # Check python library version
    INTEGRATION_VERSION = "1.4"
    assert version('wirepas_dlms_tool') == INTEGRATION_VERSION, \
        f"Wirepas DLMS tool Python must be version {INTEGRATION_VERSION}, but found {version('wirepas_dlms_tool')}"

    # Set up the logs
    log_filename = "provisioning.log"
    print(f"Additonal information on the DLMS exchanges can be found in the following script logs: {log_filename}")

    logging.basicConfig(
        filename=log_filename,
        format='%(asctime)s | [%(levelname)s] %(filename)s:%(lineno)d:%(funcName)s:%(message)s',
        level="DEBUG")

    # Prepare the connection to the wirepas network.
    wni = WirepasNetworkInterface(MQTT_HOST, MQTT_PORT, MQTT_USERNAME,
                                  MQTT_PASSWORD, strict_mode=False)

    dni = DLMSNetworkInterface(wni=wni, default_NIC_status_cb=provisioning_cb, network=NETWORK_ADDRESS)

    input("Press 'Enter' keyword to stop the provisioning script.\n")
