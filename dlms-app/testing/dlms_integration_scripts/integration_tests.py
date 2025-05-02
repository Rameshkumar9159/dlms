#!/usr/bin/env python3

# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging

import argparse
from argparse import RawTextHelpFormatter
from copy import deepcopy
from datetime import datetime, timedelta
from enum import Enum, IntEnum
from importlib.metadata import version
import json
from time import sleep, time
from threading import Thread
from typing import Any, Dict, List

from gurux_dlms.enums import RequestTypes
from gurux_dlms import GXByteBuffer, GXReplyData
from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import AssociationLevelEnum, ConnectionStatusEnum, Client, \
        DestinationEndpointEnum, DLMSNetworkInterface, ErrorCodeEnum, \
        MeterConfiguration, Meter, NicStatusWord, NotificationObisEnum, ProfileGeneric, \
        Response, SourceEndpointEnum, WirepasNotification

from utils.dlms_test_executor import Result, TestExecutor


# Tags used to store the meter settings and restore them as it was before each tests:
CONFIGURATION_TAG = "configuration"
METER_IC_TAG = "invocation_counter"
METER_SERIAL_NUMBER_TAG = "serial_number"
NIC_SYSTEM_TITLE_TAG = "nic_system_title"
SAME_AUTH_ENC_KEYS_TAG = "same_auth_enc_keys"
_PUSH_ENABLE_TAG = "push_enable_configuration"

# Response timeout in seconds allowed when sending a request to the nic or to the meter.
RESPONSE_TIMEOUT_S = 30


# Mapping between the data notification and their expected endpoints.
notifications_dest_ep: dict = {
    NotificationObisEnum.NIC_STATUS_WORD: DestinationEndpointEnum.NIC_STATUS_WORD_PUSH.endpoint,
    NotificationObisEnum.EVENT_STATUS_WORD_PUSH: DestinationEndpointEnum.ESW_NOTIFICATION.endpoint,
    NotificationObisEnum.NAME_PLATE_DETAILS: DestinationEndpointEnum.NAME_PLATE_DETAILS.endpoint,
    NotificationObisEnum.INSTANTANEOUS_PROFILE: DestinationEndpointEnum.INSTANTANEOUS_PROFILE.endpoint,
    NotificationObisEnum.BLOCK_LOAD_PROFILE: DestinationEndpointEnum.BLOCK_LOAD_PROFILE.endpoint,
    NotificationObisEnum.DAILY_LOAD_PROFILE: DestinationEndpointEnum.DAILY_LOAD_PROFILE.endpoint,
    NotificationObisEnum.BILLING_PROFILE: DestinationEndpointEnum.BILLING_PROFILE.endpoint,
    NotificationObisEnum.EXPORT_BILLING_PROFILE: DestinationEndpointEnum.EXPORT_BILLING_PROFILE.endpoint,

    # Event notifications
    NotificationObisEnum.VOLTAGE_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint,
    NotificationObisEnum.CURRENT_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint,
    NotificationObisEnum.POWER_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint,
    NotificationObisEnum.TRANSACTION_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint,
    NotificationObisEnum.OTHER_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint,
    NotificationObisEnum.NON_ROLLOVER_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint,
    NotificationObisEnum.CONTROL_EVENTS_LOG_PROFILE: DestinationEndpointEnum.EVENT_LOGS.endpoint
}


# Credentials to be set in the meters during the tests changing the credentials.
TEST_MR_PASSWORD: str = b"testmrpw".hex()
TEST_US_PASSWORD: str = b"test_us_password".hex()
TEST_FU_PASSWORD: str = b"test_fu_password".hex()
TEST_ENCRYPTION_KEY: str = b"test_encrypt_key".hex()
TEST_AUTHENTICATION_KEY: str = b"test_authent_key".hex()
TEST_WRONG_SYSTEM_TITLE: bytes = b"wrong_st"


# Endpoint to be set in the configuration in the tests with a wrong endpoint.
TEST_WRONG_ENDPOINT: int = 200


# Function to load the configuration file
def load_config(config_file: str) -> dict:
    try:
        with open(config_file, "r") as file:
            return json.load(file)
    except FileNotFoundError:
        raise FileNotFoundError(f"Configuration file '{config_file}' not found.")
    except json.JSONDecodeError as e:
        raise ValueError(f"Error decoding JSON from '{config_file}': {e}")


# Function to initialize global settings and configurations
def initialize_globals(config: dict):
    global MQTT_SETTINGS, WIREPAS_SETTINGS, DEFAULT_CONFIGURATION, METER_CONFIGURATION, METER_CONFIGURATIONS

    # MQTT settings
    MQTT_SETTINGS = {
        "host": config["MQTT_HOST"],
        "port": config["MQTT_PORT"],
        "username": config["MQTT_USERNAME"],
        "password": config["MQTT_PASSWORD"]
    }

    # Wirepas settings
    WIREPAS_SETTINGS = {
        "gateway": config.get("WIREPAS_GATEWAY"),
        "sink": config.get("WIREPAS_SINK")
    }

    # Default configuration
    DEFAULT_CONFIGURATION = MeterConfiguration(
        key_encryption_key=config.get("default_configuration", {}).get("key_encryption_key"),
        authentication_key=config.get("default_configuration", {}).get("authentication_key"),
        block_cipher_key=config.get("default_configuration", {}).get("block_cipher_key"),
        mr_password=config.get("default_configuration", {}).get("mr_password"),
        us_password=config.get("default_configuration", {}).get("us_password"),
        fu_password=config.get("default_configuration", {}).get("fu_password")
    )

    # Meter configuration
    METER_CONFIGURATION = MeterConfiguration(
        key_encryption_key=config.get("meter_configuration", {}).get("key_encryption_key"),
        authentication_key=config.get("meter_configuration", {}).get("authentication_key"),
        block_cipher_key=config.get("meter_configuration", {}).get("block_cipher_key"),
        mr_password=config.get("meter_configuration", {}).get("mr_password"),
        us_password=config.get("meter_configuration", {}).get("us_password"),
        fu_password=config.get("meter_configuration", {}).get("fu_password")
    )

    # Meter configurations
    METER_CONFIGURATIONS = {
        int(node_id): {
            CONFIGURATION_TAG: METER_CONFIGURATION,
            NIC_SYSTEM_TITLE_TAG: Meter.generate_system_title(
                meter_info["manufacturer_flag"],
                int(node_id)
            ),
            METER_SERIAL_NUMBER_TAG: bytes(meter_info["meter_serial_number"], "utf-8"),
            METER_IC_TAG: meter_info["meter_ic"],
            SAME_AUTH_ENC_KEYS_TAG: meter_info.get("same_auth_enc_keys", True)
        }
        for node_id, meter_info in config["meters"].items()
    }


class TestTag(Enum):
    """ Tests tag to know if the tests need to be executes. """
    # All tests that are not present in the test to exclude.
    EXECUTE_ALL_TESTS = "all"

    # Tag for tests related to the provisioning of the NIC sever.
    PROVISIONING_TESTS = "provisioning"

    # Tag for tests that listen to the first connection profile pushes.
    PROFILE_PUSH_ON_FIRST_CONNECTION_TESTS = "first_connection_profiles"

    # Tag for tests that verify the connection with the NIC server and to the meter.
    CONNECTION_TESTS = "connection"

    # Tag for tests to get supported obis list from the NIC server.
    NIC_SUPPORTED_OBIS_TESTS = "nic_supported_obis"

    # Tag for security access related tests of the NIC server.
    SECURITY_ACCESS_TESTS = "security_access"

    # Tag for tests that change the credentials of the meter.
    CREDENTIALS_TESTS = "credentials"

    # Tag for tests sending other basic objects queries in passthrough and to the NIC.
    BASIC_OBJ_REQUESTS_TESTS = "basic_obj_requests"

    # Tag to query meter profile generic for environment understandings (No tests execution).
    PROFILE_GENERIC_QUERIES = "pg_queries"


class DataNotificationListenResult(IntEnum):
    """ Enumerate to describe the result of the listen of a data notification. """
    # The data notification message has been received, decrypted and has data.
    RES_OK = 0

    # The data notification message is not encrypted.
    RES_NO_ENCRYPTION = 1

    # The data notification message has no data.
    RES_NO_DATA = 2

    # The data notification has not been received.
    RES_NOT_RECEIVED = 3

    # The data notification message has been received,
    # decrypted and has data but has an erroneous endpoint.
    RES_ERRONEOUS_EP = 4


# Settings to be set in the meters for the set.
unsecured_client = Client()


def recreate_meter(node_id):
    """
    Generate a new meter object. It can be called before the tests so that
    the informations from the previous test are erased.
    The MQTT settings, the invocation counter, the invoke id and the system title
    are kept from the previous meter object instance.
    """
    global WIREPAS_SETTINGS

    if node_id not in meter_informations:
        raise ValueError(f"No meter informations have been found for {node_id}!")

    meter = dni.get_meter(node_id)
    if meter:
        system_title = meter.nic_system_title
        gateway_id = meter.gateway_id
        sink_id = meter.sink_id
        dni.delete_meter(node_id)
    else:
        system_title = meter_informations[node_id][NIC_SYSTEM_TITLE_TAG]

        gateway_id = WIREPAS_SETTINGS["gateway"]
        sink_id = WIREPAS_SETTINGS["sink"]

    # Create the meter from scratch with standard parameters.
    new_meter = dni.create_meter(
        node_id=node_id,
        meter_configuration=meter_informations[node_id][CONFIGURATION_TAG],
        nic_system_title=system_title,
        gateway=gateway_id,
        sink=sink_id,
        response_timeout_s=RESPONSE_TIMEOUT_S
    )

    # Get the previous invocation counter and invoke id from the previous meter object.
    if meter:
        new_meter.set_invocation_counter(meter.invocation_counter)
        new_meter.set_invoke_id(meter.invoke_id)
    elif METER_IC_TAG in meter_informations[node_id]:
        new_meter.set_invocation_counter(meter_informations[node_id][METER_IC_TAG])

    return new_meter


def set_credentials(meter, mr_password=None, us_password=None, fu_password=None,
                    authentication_key=None, global_unicast_enc_key=None, key_encryption_key=None) -> bool:
    """
    Send a request to the NIC to change the credentials.
    Return True if the credentials of the NIC are changed, False otherwise.
    """
    try:
        if not meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
            return False

        resp = meter.set_NIC_security_material_with_list(
            mr_password=mr_password, us_password=us_password, fu_password=fu_password,
            global_unicast_enc_key=global_unicast_enc_key,
            authentication_key=authentication_key,
            key_encryption_key=key_encryption_key
        )

        if resp.error_code != ErrorCodeEnum.RES_OK:
            logging.error("The credentials could not be set.")
            meter.release_AA_NIC()
            return False

        logging.info("The credentials has been set.")
        release_result = meter.release_AA_NIC()

        logging.info("Wait 2 minutes max for a notification after setting the keys "
                     "in the meter as the NIC will reboot.")
        meter.get_next_message(120)
        return release_result
    except Exception as exception:
        logging.exception(exception)
        return False


def request_notification(meter, notification: NotificationObisEnum, event_log_type: int = None):
    """ Request the NIC with a test specific command
    to get immediately profile generic notifications.

    Args:
        meter: Meter being tested.
        notification: Notification to get immediately from the meter.
        event_log_type: Event log type of the notification if it is an event logs.
    """
    class TestCommandEnum(IntEnum):
        """ Enumerate all NIC test command ids. """
        TEST_CMD_INSTANT_FORCE_PUSH = 2
        TEST_CMD_BLOCK_LOAD_FORCE_PUSH = 3
        TEST_CMD_DAILY_LOAD_FORCE_PUSH = 5
        TEST_CMD_BILLING_FORCE_PUSH = 7
        TEST_CMD_EVT_LOG_FORCE_PUSH = 9

    test_immediate_ep = 66  # Endpoints to use for the test commands

    if notification == NotificationObisEnum.INSTANTANEOUS_PROFILE:
        command = TestCommandEnum.TEST_CMD_INSTANT_FORCE_PUSH
    elif notification == NotificationObisEnum.BLOCK_LOAD_PROFILE:
        command = TestCommandEnum.TEST_CMD_BLOCK_LOAD_FORCE_PUSH
    elif notification == NotificationObisEnum.DAILY_LOAD_PROFILE:
        command = TestCommandEnum.TEST_CMD_DAILY_LOAD_FORCE_PUSH
    elif notification == NotificationObisEnum.BILLING_PROFILE:
        command = TestCommandEnum.TEST_CMD_BILLING_FORCE_PUSH
    elif notification in (
        NotificationObisEnum.VOLTAGE_EVENTS_LOG_PROFILE,
        NotificationObisEnum.CURRENT_EVENTS_LOG_PROFILE,
        NotificationObisEnum.POWER_EVENTS_LOG_PROFILE,
        NotificationObisEnum.TRANSACTION_EVENTS_LOG_PROFILE,
        NotificationObisEnum.OTHER_EVENTS_LOG_PROFILE,
        NotificationObisEnum.NON_ROLLOVER_EVENTS_LOG_PROFILE,
        NotificationObisEnum.CONTROL_EVENTS_LOG_PROFILE
    ):
        command = TestCommandEnum.TEST_CMD_EVT_LOG_FORCE_PUSH
    else:
        raise ValueError(f"No command exists to generate immediatly {notification.obis_name}!")

    if command != TestCommandEnum.TEST_CMD_EVT_LOG_FORCE_PUSH:
        request_payload = struct.pack('<B', command)
    else:
        request_payload = struct.pack('<BB', command, event_log_type)

    logging.info("Send test command hex: '%s'", request_payload.hex())
    wni.send_message(gw_id=meter.gateway_id, sink_id=meter.sink_id, dest=meter.node_id,
                     src_ep=test_immediate_ep, dst_ep=test_immediate_ep,
                     payload=request_payload)


def listen_notification(meter, notification_obis: NotificationObisEnum,
                        timeout_s: int) -> DataNotificationListenResult:
    """ Listen to the meter to get the first readable notification listened to.

    Args:
        meter: Meter to test.
        notification_obis: Notification obis to listen to.
        timeout_s: Timeout in seconds to wait for notifications.
    """
    logging.info("Listening to the meter for %ds for %s notification.",
                 timeout_s, notification_obis.obis_name)
    time_start = int(time())

    # Get messages one by one from the meter
    while True:
        time_left = (time_start + timeout_s) - int(time())

        # Wait for a message to be received for the time left.
        message = meter.get_next_message(time_left)
        if not message.data:
            break

        # Try to get the notification data with an unsecured client.
        unsecured_notification = WirepasNotification.from_payload(unsecured_client, message.data)
        if unsecured_notification:
            obis = NotificationObisEnum.from_obis_code(unsecured_notification.obis_code)
            if obis == notification_obis.obis_name:
                logging.error("The notification %s (%s) was received without encryption from %d.",
                              obis.obis_code, obis.obis_name, meter.node_id)
                logging.info(unsecured_notification.xml)
                return DataNotificationListenResult.RES_NO_ENCRYPTION

        # Try to get the notification data with the real client.
        notification = WirepasNotification.from_payload(meter.notification_client, message.data)
        if notification:
            obis = NotificationObisEnum.from_obis_code(notification.obis_code)
            if obis != notification_obis:
                continue

            if not notification.value:
                logging.warning("The notification %s (%s) has been decrypted successfully from %d, "
                                "but has not data!", obis.obis_code, obis.obis_name, meter.node_id)
                if notification.xml:
                    logging.info(notification.xml)
                return DataNotificationListenResult.RES_NO_DATA
            else:
                logging.info("The notification %s (%s) has been decrypted successfully from %d!",
                             obis.obis_code, obis.obis_name, meter.node_id)
                logging.info(notification.xml)

                if message.src_ep_value != SourceEndpointEnum.ENERGY_METER_PUSH_NOTIFICATION.endpoint \
                        or message.dst_ep_value != notifications_dest_ep[notification_obis]:
                    return DataNotificationListenResult.RES_ERRONEOUS_EP

                return DataNotificationListenResult.RES_OK

    # Replace no_result test result to False as they timed out.
    logging.error("A timeout occured while waiting for %s, as no notification has been received from %d!",
                  notification_obis.obis_name, meter.node_id)

    return DataNotificationListenResult.RES_NOT_RECEIVED


# Decorator functions to be called after the tests to set back the meter systems to their normal settings.
def credential_change_test_dec(fn):
    """ Decorator to set back meter credentials before finishing the test case. """

    def set_back_meter_credentials(meter) -> bool:
        """ Set back meter credentials of a meter after a test. """
        logging.info("Trying to set back the credentials.")
        former_configuration = meter_informations[meter.node_id].get(CONFIGURATION_TAG, None)
        actual_configuration = meter.meter_configuration
        if not former_configuration:
            logging.warning("No configuration has been found for node %d.", meter.node_id)
            return False

        credentials_to_set_back = {}
        if former_configuration.authentication_key and \
                former_configuration.authentication_key != actual_configuration.authentication_key:
            credentials_to_set_back["authentication_key"] = former_configuration.authentication_key
        if former_configuration.block_cipher_key and \
                former_configuration.block_cipher_key != actual_configuration.block_cipher_key:
            credentials_to_set_back["global_unicast_enc_key"] = former_configuration.block_cipher_key
        if former_configuration.key_encryption_key and \
                former_configuration.key_encryption_key != actual_configuration.key_encryption_key:
            credentials_to_set_back["key_encryption_key"] = former_configuration.key_encryption_key
        if former_configuration.mr_password and \
                former_configuration.mr_password != actual_configuration.mr_password:
            credentials_to_set_back["mr_password"] = former_configuration.mr_password
        if former_configuration.us_password and \
                former_configuration.us_password != actual_configuration.us_password:
            credentials_to_set_back["us_password"] = former_configuration.us_password
        if former_configuration.fu_password and \
                former_configuration.fu_password != actual_configuration.fu_password:
            credentials_to_set_back["fu_password"] = former_configuration.fu_password

        if not credentials_to_set_back:
            logging.warning("No credentials need to be set back for %d!", meter.node_id)
            return False

        set_cred_res = set_credentials(meter, **credentials_to_set_back)
        if not set_cred_res:
            logging.critical("Meter %d credentials could not be set back. ", meter.node_id)

        return False

    def wrapper(meter, *args, **kwargs) -> bool:
        result = fn(meter, *args, **kwargs)
        are_keys_set_back = set_back_meter_credentials(meter)
        logging.debug("Actual meter configuration in the NIC: \n%s", meter.meter_configuration)
        return result and are_keys_set_back

    return wrapper


def push_enable_tests_dec(fn):
    """ Decorator to set back meter push enable when finishing the test case. """
    def wrapper(self, node_id, *args, **kwargs):
        fn(self, node_id, *args, **kwargs)

        # Set back the push enable configuration.
        former_push_config = meter_informations[node_id].get(_PUSH_ENABLE_TAG, None)
        meter = recreate_meter(node_id)
        logging.info("Set back former NIC push enable configuration value.")
        if former_push_config and meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
            try:
                resp = meter.set_NIC_push_enable_configuration(former_push_config)
                if meter.release_AA_NIC() and resp.error_code == ErrorCodeEnum.RES_OK:
                    logging.info("The former NIC push enable configuration value has been set back.")
                    return True
            except Exception as exception:
                logging.exception(exception)

        logging.warning("The push enable configuration for meter %s could not be set back.", meter.node_id)

    return wrapper


def changing_system_title_test_dec(fn):
    """ Callback to set back nic system title when finishing the test case. """
    def wrapper(meter, *args, **kwargs) -> Result:
        nic_system_title = meter.nic_system_title
        result = fn(meter, *args, **kwargs)
        meter.nic_system_title = nic_system_title
        return result

    return wrapper


# Test functions to be executed.
def nic_status_word_condition(meter) -> Result:
    """ Verify the NIC status word test conditions. """
    if meter_informations[meter.node_id].get(METER_SERIAL_NUMBER_TAG, None) is None:
        logging.error("The serial number must be provided for the NIC status word!")
        return Result(False, "No serial number has been provided!")

    return Result(True)


def nic_status_word_test(meter, timeout_s=300) -> Result:
    """ Test that the NIC status word is sent when the NIC is not provisioned. """
    test_result = None

    def nic_status_cb(meter_listened: Meter, nic_status: NicStatusWord):
        """ Temporary NIC status cb of the meter. """
        expected_serial_number = meter_informations[meter.node_id][METER_SERIAL_NUMBER_TAG]
        if meter_listened != meter:
            return

        nonlocal test_result
        if nic_status.PC_connection_status != ConnectionStatusEnum.TESTED_SUCCESSFUL or \
                nic_status.US_connection_status != ConnectionStatusEnum.TESTED_UNSUCCESSFUL:
            logging.warning("Meter %d sent a NIC status but it doesn't ask to be provisioned!", meter.node_id)
            return

        if nic_status.serial_number != expected_serial_number:
            logging.error("Serial Number expected to be %s (%s) but found %s (%s)",
                          expected_serial_number, expected_serial_number.hex(),
                          nic_status.serial_number, nic_status.serial_number.hex())
            test_result = Result(False, "wrong serial number")
        elif nic_status.src_ep.enum != SourceEndpointEnum.ENERGY_METER_PUSH_NOTIFICATION \
                or nic_status.dst_ep.enum != DestinationEndpointEnum.NIC_STATUS_WORD_PUSH:
            test_result = Result(True, f"Incorrect endpoints ({nic_status.src_ep.value}, {nic_status.dst_ep.value})")
        else:
            test_result = Result(True)

    meter.update_meter(NIC_status_cb=nic_status_cb)
    logging.info("Waiting %ds for a NIC status word!", timeout_s)
    start = time()
    while test_result is None and time() - start < timeout_s:  # Wait for NIC status word notifications.
        sleep(0.1)

    meter.update_meter(NIC_status_cb=None)
    return Result(False) if test_result is None else test_result


def provisioning_test(meter) -> Result:
    """ Test that the NIC can be provisioned. """
    global DEFAULT_CONFIGURATION
    meter.update_meter(meter_configuration=DEFAULT_CONFIGURATION,
                       response_timeout_s=RESPONSE_TIMEOUT_S)

    configuration_to_set = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Provisioning the meter %d", meter.node_id)
    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        res_set_keys = meter.set_NIC_security_material_from_config(configuration_to_set)

        # Do the registration
        get_res = meter.get_NIC_data(obis_code="0.0.96.0.1.255")
        if get_res.error_code == ErrorCodeEnum.RES_OK:
            registration_result = meter.set_NIC_registration_status(True).error_code
            if not registration_result:
                return Result(False, "The registration failed!")

        result_release = meter.release_AA_NIC()

        if not result_release:
            logging.error("The association could not be released succesfully!")
            return Result(False)
        elif res_set_keys.error_code == ErrorCodeEnum.RES_OK:
            logging.info("The meter %d has been provisioned!", meter.node_id)

            return Result(True)
        else:
            return Result(False, "The provisioning failed!")

    return Result(False, "Failed to establishing connection with the NIC in US!")


def listen_name_plate_on_first_connection(meter, timeout_s=120) -> Result:
    result = listen_notification(meter, NotificationObisEnum.NAME_PLATE_DETAILS, timeout_s)
    if result == DataNotificationListenResult.RES_ERRONEOUS_EP:
        return Result(True, "Erroneous endpoint")

    return Result(result == DataNotificationListenResult.RES_OK)


def connection_nic_pc(meter) -> Result:
    """ Test the PC association with the NIC server. """
    logging.info("Testing the PC association with a NIC.")

    if not meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        return Result(False, "The AA establishment in PC with the NIC failed!")

    resp = meter.get_NIC_invocation_counter()
    if resp.value:
        meter_informations[meter.node_id][METER_IC_TAG] = resp.value

    released = meter.release_AA_NIC()

    if resp.error_code == ErrorCodeEnum.RES_OK and resp.value is not None:
        logging.info("The connection in PC with the NIC is successful!")
        return Result(True)
    elif not released:
        return Result(False, "The association could not be released succesfully!")
    else:
        return Result(False, "The request has not be answered by the NIC!")


def connection_nic_mr(meter) -> Result:
    """ Test the MR association with NIC server. """
    logging.info("Testing the MR association with a NIC.")

    # Verify that AA can't be established.
    if not meter.establish_AA_NIC(AssociationLevelEnum.MR_ASSOCIATION):
        logging.info("Connection with NIC in MR association successfully failed!")
        return Result(True)

    logging.warning("Connection with NIC in MR association was successful!")
    meter.release_AA_NIC()
    return Result(False, "MR connection has been established with NIC!")


def connection_nic_us(meter) -> Result:
    """ Test the US association with the NIC server. """
    logging.info("Testing the US association with a NIC.")

    if not meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        return Result(False, "AA could not be established with the NIC in US association.")

    resp = meter.get_NIC_push_enable_configuration()
    if resp.value:
        meter_informations[meter.node_id][_PUSH_ENABLE_TAG] = resp.value.value

    released = meter.release_AA_NIC()

    if resp.error_code == ErrorCodeEnum.RES_OK and resp.value is not None:
        logging.info("The connection in US with the NIC is successful!")
        return Result(True)
    elif not released:
        return Result(False, "The association could not be released succesfully!")
    else:
        return Result(False, "The request has not be answered by the NIC!")


def connection_nic_fu(meter) -> Result:
    """ Test the FU association with the NIC server. """
    logging.info("Testing the FU association with a NIC.")

    # Verify that AA can't be established.
    if not meter.establish_AA_NIC(AssociationLevelEnum.FU_ASSOCIATION):
        logging.info("Connection with NIC in FU association successfully failed!")
        return Result(True)

    logging.warning("Connection with NIC in FU association was successful!")
    meter.release_AA_NIC()
    return Result(False, "A FU connection has been established with NIC!")


def connection_pt_pc(meter) -> Result:
    """ Test the PC association connection in passthrough. """
    logging.info("Testing the passthrough connection in PC association with a meter.")
    resp = meter.get_meter_serial_number(connection_type=AssociationLevelEnum.PC_ASSOCIATION)
    if resp.error_code == ErrorCodeEnum.RES_OK:
        if resp.value:
            logging.info("The connection in PC in passthrough is successful!")
            return Result(True)
        else:
            return Result(False, "Empty response")
    else:
        logging.info("The connection in PC in passthrough failed!")
        return Result.from_response_error_code(resp.error_code)


def connection_pt_mr(meter) -> Result:
    """ Test the MR association connection in passthrough. """
    logging.info("Testing the passthrough connection in MR association with a meter.")
    resp = meter.get_meter_serial_number(connection_type=AssociationLevelEnum.MR_ASSOCIATION)
    if resp.error_code == ErrorCodeEnum.RES_OK:
        if resp.value:
            logging.info("The connection in MR in passthrough is successful!")
            return Result(True)
        else:
            return Result(False, "Empty response")
    else:
        logging.info("The connection in MR in passthrough failed!")
        return Result.from_response_error_code(resp.error_code)


def connection_pt_us(meter) -> Result:
    """ Test the US association connection in passthrough. """
    logging.info("Testing the passthrough connection in US association with a meter.")
    resp = meter.get_meter_serial_number(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    if resp.error_code == ErrorCodeEnum.RES_OK:
        if resp.value:
            logging.info("The connection in US in passthrough is successful!")
            return Result(True)
        else:
            return Result(False, "Empty response")
    else:
        logging.info("The connection in US in passthrough failed!")
        return Result.from_response_error_code(resp.error_code)


def connection_pt_fu(meter) -> Result:
    """ Test the FU association connection in passthrough. """
    logging.info("Testing the passthrough connection in FU association with a meter.")
    resp = meter.get_meter_image_transfer_attrib(attribute_id=5)
    if resp.error_code == ErrorCodeEnum.RES_OK:
        if resp.value:
            logging.info("The connection in FU in passthrough is successful!")
            return Result(True)
        else:
            return Result(False, "Empty response")
    else:
        logging.info("The connection in FU in passthrough failed!")
        return Result.from_response_error_code(resp.error_code)


def get_nic_pc_supported_objects(meter) -> Result:
    """ Get all supported COSEM objects for the NIC server PC association. """
    if not meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        return Result(False)

    resp = meter.get_NIC_list_supported_obis()
    return Result(meter.release_AA_NIC() and resp.error_code == ErrorCodeEnum.RES_OK and resp.value)


def get_nic_us_supported_objects(meter) -> Result:
    """ Get all supported COSEM objects for the NIC server US association. """
    if not meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        return Result(False)

    resp = meter.get_NIC_list_supported_obis()
    return Result(meter.release_AA_NIC() and resp.error_code == ErrorCodeEnum.RES_OK and resp.value)


def secu_access_pt_pc_privilege_escalation(meter):
    """ Test privilege accesses of PC association in passthrough. """
    resp = meter.get_meter_instantaneous_profile(connection_type=AssociationLevelEnum.PC_ASSOCIATION)
    return Result(resp.error_code != ErrorCodeEnum.RES_OK or not resp.value)


def secu_access_nic_pc_privilege_escalation(meter):
    """ Test privilege accesses of PC association with the NIC server. """
    if meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        conf = meter.get_NIC_push_enable_configuration()
        if conf.error_code != ErrorCodeEnum.RES_OK:
            meter.release_AA_NIC()
            return Result(True)

        resp = meter.set_NIC_push_enable_configuration(conf.value.value)
        return Result(meter.release_AA_NIC() and resp.error_code != ErrorCodeEnum.RES_OK)


def secu_access_pt_mr_privilege_escalation(meter):
    """ Test privilege accesses of MR association in passthrough. """
    period_resp = meter.get_meter_block_load_capture_period(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    if not period_resp.value:
        logging.error("Meter block load period could not be retrieved in passthrough with MR association.")
        return Result(False)

    logging.info("Try to set the meter block load period in passthrough with MR association.")
    set_resp = meter.set_meter_block_load_capture_period(value_to_set=period_resp.value, connection_type=AssociationLevelEnum.MR_ASSOCIATION)
    return Result(set_resp.error_code != ErrorCodeEnum.RES_OK)


def secu_access_pt_mr_unsecured_req(meter):
    """ Sending unsecured message in MR association in passthrough should not have unsecured response. """
    meter.meter_configuration = MeterConfiguration()
    set_resp = meter.get_meter_instantaneous_profile(connection_type=AssociationLevelEnum.MR_ASSOCIATION)
    return Result(set_resp.error_code != ErrorCodeEnum.RES_OK or set_resp.value is None)


def secu_access_pt_fu_privilege_escalation(meter):
    """ Test privilege accesses of FU association in passthrough. """
    period_resp = meter.get_meter_block_load_capture_period(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    if not period_resp.value:
        logging.error("Meter block load period could not be retrieved in passthrough with US association.")
        return Result(False)

    logging.info("Try to set the meter block load period in passthrough with FU association.")
    set_resp = meter.set_meter_block_load_capture_period(value_to_set=period_resp.value, connection_type=AssociationLevelEnum.FU_ASSOCIATION)
    return Result(set_resp.error_code != ErrorCodeEnum.RES_OK)


def secu_access_pt_us_unsecured_req(meter):
    """ Sending unsecured message in US association in passthrough should not have unsecured response. """
    meter.meter_configuration = MeterConfiguration()
    set_resp = meter.get_meter_instantaneous_profile(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return Result(set_resp.error_code != ErrorCodeEnum.RES_OK or set_resp.value is None)


def secu_access_pt_fu_unsecured_req(meter):
    """ Sending unsecured message in FU association in passthrough should not have unsecured unsecured response. """
    meter.meter_configuration = MeterConfiguration()
    resp = meter.get_meter_list_supported_obis(connection_type=AssociationLevelEnum.FU_ASSOCIATION)
    return Result(resp.error_code != ErrorCodeEnum.RES_OK or resp.value is None)


def secu_access_nic_us_unsecured_req(meter) -> Result:
    """ Sending unsecured message in US association with the NIC server should not have unsecured response. """
    if not meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        return Result(False, "AA establishment with NIC failed in US association!")

    meter.meter_configuration = MeterConfiguration()
    conf = meter.get_NIC_push_enable_configuration()
    if conf.error_code != ErrorCodeEnum.RES_OK:
        meter.release_AA_NIC()
        return Result(True)

    resp = meter.set_NIC_push_enable_configuration(conf.value.value)
    return Result(meter.release_AA_NIC() and resp.error_code != ErrorCodeEnum.RES_OK)


def secu_access_nic_us_unsecured_aa(meter):
    """ Try to establish AA with NIC server in US association without using ciphering. """
    def establish_unsecure_us_aa(meter):
        # Create a PC association client with NIC US addresses.
        connection_type = AssociationLevelEnum.PC_ASSOCIATION
        logging.info("Establishing unsecure AA with %d with US association.", meter.node_id)
        client = meter._set_new_client(connection_type, NIC_server=True)

        # Set NIC US DLMS addresses to a unsecured PC association client.
        meter.NIC_client.set_addresses(client_address=0x30, server_address=100)
        gx_client = client.gx_client

        # Send unsecure AARQ request.
        logging.info("AARQ request is sent to %d.", meter.node_id)
        aare_response = meter._aarq_request()
        if not aare_response.payload:
            logging.info("No valid response was received from the AARQ request.")
            return False

        # Parse the aare response.
        logging.info("AARE response from %d is being parsed.", meter.node_id)
        reply = GXReplyData(RequestTypes.DATABLOCK)
        gx_client.getData(GXByteBuffer(aare_response.payload), reply, None)
        gx_client.parseAareResponse(reply.data)
        return True

    meter.meter_configuration = MeterConfiguration()
    if establish_unsecure_us_aa(meter):
        conf = meter.get_NIC_push_enable_configuration()
        if conf.error_code != ErrorCodeEnum.RES_OK:
            meter.release_AA_NIC()
            return Result(True, "An unsecured US association as been established "
                          "with the NIC but the request was not answered unencrypted!")

        resp = meter.set_NIC_push_enable_configuration(conf.value.value)
        return Result(meter.release_AA_NIC() and resp.error_code != ErrorCodeEnum.RES_OK)

    return Result(True)


def secu_access_pt_wrong_invocation_counter(meter) -> Result:
    """ Send request with a wrong invocation counter in passthrough. """
    logging.info("Set the invocation counter to %d temporarily", meter.invocation_counter - 1)
    meter.set_invocation_counter(meter.invocation_counter - 1)
    resp = meter.get_meter_serial_number(connection_type=AssociationLevelEnum.US_ASSOCIATION)

    if resp.error_code == ErrorCodeEnum.RES_OK and resp.value is not None:
        logging.error("A passthrough request with the wrong IC has been answered in US association.")
        return Result(False)

    return Result(True)


def secu_access_nic_wrong_invocation_counter(meter) -> Result:
    """ Send request with a wrong invocation counter to the NIC server. """
    logging.info("Set the invocation counter to %d temporarily", meter.invocation_counter - 1)
    meter.set_invocation_counter(meter.invocation_counter - 1)
    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        logging.warning("Trying to establish AA in US association with NIC server "
                        "with a wrong IC should have returned an error.")
        meter.release_AA_NIC()
        return Result(False)

    # IC is back to normal now.
    if not meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        return Result(False)

    logging.info("Set the invocation counter to %d temporarily", meter.invocation_counter - 1)
    meter.set_invocation_counter(meter.invocation_counter - 1)
    resp = meter.get_NIC_push_enable_configuration()
    released = meter.release_AA_NIC()

    if resp.error_code == ErrorCodeEnum.RES_OK and resp.value is not None:
        return Result(False, "A request with the wrong IC has been answered by the NIC in US association.")
    elif not released:
        sleep(60)
        return Result(True, "The association could not be released succesfully!")

    return Result(True)


@changing_system_title_test_dec
def secu_access_pt_wrong_system_title(meter) -> Result:
    """ Send request with a wrong system title in passthrough. """
    logging.info("Set the system title temporarily to %s", TEST_WRONG_SYSTEM_TITLE)
    meter.nic_system_title = TEST_WRONG_SYSTEM_TITLE
    resp = meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION)
    return Result(resp.error_code != ErrorCodeEnum.RES_OK or resp.value is None)


def request_nic_increment_ic(meter) -> Result:
    """ Send a simple request to the NIC server with an invocation counter incremented by 50. """
    logging.info("Add 50 to the invocation counter.")
    meter.invocation_counter += 50

    if meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        logging.info("Add 50 to the invocation counter.")
        meter.invocation_counter += 50
        ic_resp = meter.get_NIC_invocation_counter()
        meter.release_AA_NIC()
        return Result(ic_resp.error_code == ErrorCodeEnum.RES_OK and ic_resp.value is not None)

    return Result(False, "Association could not be established!")


def request_pt_increment_ic(meter) -> Result:
    """ Send a simple request in passthrough with an invocation counter incremented by 50. """
    logging.info("Add 50 to the invocation counter.")
    meter.invocation_counter += 50
    clock_resp = meter.get_meter_clock(AssociationLevelEnum.US_ASSOCIATION)
    return Result(clock_resp.error_code == ErrorCodeEnum.RES_OK and clock_resp.value is not None)


def verification_us_nic_set_pw(meter) -> Result:
    """ Setting US password should influence NIC US association. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Query meter in NIC US association with former US password.")

    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        logging.error("Trying to establish AA in US association with former password should have returned an error.")
        meter.release_AA_NIC()
        return Result(False, "NIC server uses with former password!")

    logging.info("OK - establishing AA in US association with former password did not work.")

    logging.info("Query meter in NIC US association with new US password.")
    meter.meter_configuration.us_password = TEST_US_PASSWORD
    return Result(meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION) and meter.release_AA_NIC())


def verification_nic_set_enc_auth(meter) -> Result:
    """ Setting encryption/authentication keys should influence NIC associations. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Establish AA with a meter in NIC US association with former keys.")

    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        logging.error("Trying to establish AA in US association with former keys should have returned an error.")
        meter.release_AA_NIC()
        return Result(False, "NIC server responses are sent with former keys!")

    logging.info("OK - Passthrough in US association with former keys did not work.")

    logging.info("Query meter in NIC US association with new keys.")
    meter.meter_configuration.authentication_key = TEST_ENCRYPTION_KEY
    meter.meter_configuration.block_cipher_key = TEST_ENCRYPTION_KEY
    return Result(meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION) and meter.release_AA_NIC())


def verification_pt_set_enc_auth(meter) -> Result:
    """ Setting encryption/authentication keys should influence passthrough. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Query meter in passthrough in US association with former keys.")

    if meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION).error_code == ErrorCodeEnum.RES_OK:
        logging.error("Query meter in passthrough in US association with former keys should have returned an error.")
        return Result(False, "Passthrough responses are sent with former keys!")

    logging.info("OK - establishing AA in US association with former keys did not work.")

    logging.info("Query meter in passthrough in US association with new keys.")
    meter.meter_configuration.authentication_key = TEST_ENCRYPTION_KEY
    meter.meter_configuration.block_cipher_key = TEST_ENCRYPTION_KEY
    return Result(meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION).error_code == ErrorCodeEnum.RES_OK)


def verification_nic_set_enc(meter) -> Result:
    """ Setting encryption key should influence NIC associations. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Establish AA with a meter in NIC US association with former encryption key.")

    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        logging.error("Trying to establish AA in US association with former encryption key should have returned an error.")
        meter.release_AA_NIC()
        return Result(False)

    logging.info("OK - Passthrough in US association with former encryption key did not work.")

    logging.info("Query meter in NIC US association with new encryption key.")
    meter.meter_configuration.block_cipher_key = TEST_ENCRYPTION_KEY
    return Result(meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION) and meter.release_AA_NIC())


def verification_pt_set_enc(meter) -> Result:
    """ Setting encryption key should influence passthrough. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Query meter in passthrough in US association with former encryption key.")

    if meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION).error_code == ErrorCodeEnum.RES_OK:
        logging.error("Query meter in passthrough in US association with former encryption key should have returned an error.")
        return Result(False)

    logging.info("OK - establishing AA in US association with former encryption key did not work.")

    logging.info("Query meter in passthrough in US association with new encryption key.")
    meter.meter_configuration.block_cipher_key = TEST_ENCRYPTION_KEY
    return Result(meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION).error_code == ErrorCodeEnum.RES_OK)


def verification_pt_set_auth(meter) -> Result:
    """ Setting authentication key should influence passthrough. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Query meter in passthrough in US association with former authentication key.")

    if meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION).error_code == ErrorCodeEnum.RES_OK:
        return Result(False)

    logging.info("OK - Passthrough in US association with former authentication key did not work.")

    logging.info("Query meter in passthrough in US association with new authentication key.")
    meter.meter_configuration.authentication_key = TEST_AUTHENTICATION_KEY

    return Result(meter.get_meter_serial_number(AssociationLevelEnum.US_ASSOCIATION).error_code == ErrorCodeEnum.RES_OK)


def verification_nic_set_auth(meter) -> Result:
    """ Setting authentication key should influence NIC associations. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]
    logging.info("Establish AA with a meter in NIC US association with former authentication key.")

    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        logging.error("Trying to establish AA in US association with former authentication key should have returned an error.")
        meter.release_AA_NIC()
        return Result(False)

    logging.info("OK - establishing AA in US association with former authentication key did not work.")

    logging.info("Query meter in NIC US association with new authentication key.")
    meter.meter_configuration.authentication_key = TEST_AUTHENTICATION_KEY

    return Result(meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION) and meter.release_AA_NIC())


def verification_pt_set_all_keys(meter) -> Result:
    """ Set all keys in a message should influence passthrough. """
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]

    logging.info("Query meter in passthrough in MR association with former keys.")
    if connection_pt_mr(meter):
        logging.error("Meter answered a request in passthrough in MR association with former keys.")
        return Result(False)

    logging.info("Query meter in passthrough in US association with former keys.")
    if connection_pt_us(meter):
        logging.error("Meter answered a request in passthrough in US association with former keys.")
        return Result(False)

    logging.info("Query meter in passthrough in FU association with former keys.")
    if connection_pt_fu(meter):
        logging.error("Meter answered a request in passthrough in FU association with former keys.")
        return Result(False)

    # Set meter former setting to verify that the passthrough do not work with former keys.
    meter.meter_configuration = MeterConfiguration(
        TEST_AUTHENTICATION_KEY, TEST_ENCRYPTION_KEY,
        meter.meter_configuration.key_encryption_key,
        TEST_MR_PASSWORD, TEST_US_PASSWORD, TEST_FU_PASSWORD)

    if not connection_pt_mr(meter):
        logging.error("Meter couldn't connect in passthrough in MR association with new keys.")
    elif not connection_pt_us(meter):
        logging.error("Meter couldn't connect in passthrough in US association with new keys.")
    elif not connection_pt_fu(meter):
        logging.error("Meter couldn't connect in passthrough in FU association with new keys.")
    else:
        return Result(True)

    return Result(False)


def verification_nic_set_all_keys(meter) -> Result:
    """ Set all keys in a message should influence NIC server associations. """
    # Set meter former setting to verify that the passthrough do not work with former keys.
    meter.meter_configuration = meter_informations[meter.node_id][CONFIGURATION_TAG]

    logging.info("Establish AA with a meter in NIC US association with former keys.")
    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        logging.error("Trying to establish AA in US association with former keys should have returned an error.")
        meter.release_AA_NIC()
        return Result(False)

    logging.info("OK - establishing AA in US association with former key did not work.")

    # Set meter new configuration to verify that the passthrough does work with new keys.
    meter.meter_configuration = MeterConfiguration(
        TEST_AUTHENTICATION_KEY, TEST_ENCRYPTION_KEY,
        meter.meter_configuration.key_encryption_key,
        TEST_MR_PASSWORD, TEST_US_PASSWORD, TEST_FU_PASSWORD)

    if not connection_nic_us(meter):
        logging.error("Meter couldn't connect to the NIC server in US association with new keys.")
        return Result(False)

    return Result(True)


@credential_change_test_dec
def change_mr_password_tests(meter):
    """ Execute MR password related tests. """
    logging.info("Verify that the MR connection in passthrough is valid before changing the MR password.")
    connection_pt_mr_res = connection_pt_mr(meter).test_result_bool
    if connection_pt_mr_res:
        logging.info("Change the MR secret.")
        set_mr_pw_res = set_credentials(meter, mr_password=TEST_MR_PASSWORD)

    test_executor.execute_test(meter, connection_pt_mr, "CREDENTIALS_MR_PW_PT",
                               connection_pt_mr_res and set_mr_pw_res)


@credential_change_test_dec
def change_us_password_tests(meter):
    """ Execute US password related tests. """
    logging.info("Verification that the US connections in passthrough and with the NIC are valid "
                 "before changing the US password.")
    connection_pt_us_res = connection_pt_us(meter).test_result_bool
    connection_nic_us_res = connection_nic_us(meter).test_result_bool
    if connection_pt_us_res or connection_nic_us_res:
        logging.info("Change the US secret.")
        set_us_pw_res = set_credentials(meter, us_password=TEST_US_PASSWORD)

    test_executor.execute_test(meter, connection_pt_us, "CREDENTIALS_US_PW_PT",
                               connection_pt_us_res and set_us_pw_res)
    test_executor.execute_test(meter, verification_us_nic_set_pw, "CREDENTIALS_US_PW_NIC",
                               connection_nic_us_res and set_us_pw_res)


@credential_change_test_dec
def change_fu_password_tests(meter):
    """ Execute FU password related tests. """
    logging.info("Verify that the FU connection in passthrough is valid before changing the FU password.")
    connection_pt_fu_res = connection_pt_fu(meter).test_result_bool
    if connection_pt_fu_res:
        logging.info("Change the FU secret.")
        set_fu_pw_res = set_credentials(meter, fu_password=TEST_FU_PASSWORD)

    test_executor.execute_test(meter, connection_pt_fu, "CREDENTIALS_FU_PW_PT",
                               connection_pt_fu_res and set_fu_pw_res)


@credential_change_test_dec
def change_enc_auth_key_tests(meter):
    """ Execute encryption/authentication keys related tests. """
    logging.info("Verification that the US connections in passthrough and with the NIC are valid "
                 "before changing the encryption key.")
    connection_pt_us_res = connection_pt_us(meter).test_result_bool
    connection_nic_us_res = connection_nic_us(meter).test_result_bool

    logging.info("Change the encryption and the authentication keys.")
    set_enc_key_res = set_credentials(meter, authentication_key=TEST_ENCRYPTION_KEY,
                                      global_unicast_enc_key=TEST_ENCRYPTION_KEY)

    test_executor.execute_test(meter, verification_nic_set_enc_auth, "CREDENTIALS_ENC_AUTH_NIC",
                               connection_nic_us_res and set_enc_key_res)
    test_executor.execute_test(meter, verification_pt_set_enc_auth, "CREDENTIALS_ENC_AUTH_PT",
                               connection_pt_us_res and set_enc_key_res)


@credential_change_test_dec
def change_encryption_key_tests(meter):
    """ Execute encryption key related tests. """
    logging.info("Verification that the US connections in passthrough and with the NIC are valid "
                 "before changing the encryption key.")
    connection_pt_us_res = connection_pt_us(meter).test_result_bool
    connection_nic_us_res = connection_nic_us(meter).test_result_bool

    logging.info("Change the encryption key.")
    set_enc_key_res = set_credentials(meter, global_unicast_enc_key=TEST_ENCRYPTION_KEY)

    test_executor.execute_test(meter, verification_nic_set_enc, "CREDENTIALS_ENC_NIC",
                               connection_nic_us_res and set_enc_key_res)
    test_executor.execute_test(meter, verification_pt_set_enc, "CREDENTIALS_ENC_PT",
                               connection_pt_us_res and set_enc_key_res)


@credential_change_test_dec
def change_authentication_key_tests(meter):
    """ Execute authentication key related tests. """
    logging.info("Verification that the US connections in passthrough and with the NIC are valid "
                 "before changing the authentication key.")
    connection_pt_us_res = connection_pt_us(meter).test_result_bool
    connection_nic_us_res = connection_nic_us(meter).test_result_bool

    if connection_pt_us_res or connection_nic_us_res:
        logging.info("Change the authentication key.")
        set_auth_key_res = set_credentials(meter, authentication_key=TEST_AUTHENTICATION_KEY)

    test_executor.execute_test(meter, verification_nic_set_auth, "CREDENTIALS_AUTH_NIC",
                               connection_pt_us_res and set_auth_key_res)
    test_executor.execute_test(meter, verification_pt_set_auth, "CREDENTIALS_AUTH_PT",
                               connection_nic_us_res and set_auth_key_res)


@credential_change_test_dec
def change_all_keys_at_once_tests(meter):
    """ Execute all keys change related tests. """
    connection_pt_mr_res = connection_pt_mr(meter).test_result_bool
    connection_pt_us_res = connection_pt_us(meter).test_result_bool
    connection_pt_fu_res = connection_pt_fu(meter).test_result_bool
    connection_nic_us_res = connection_nic_us(meter).test_result_bool

    logging.info("Change all the keys.")
    set_all_keys_res = set_credentials(
        meter=meter,
        mr_password=TEST_MR_PASSWORD,
        us_password=TEST_US_PASSWORD,
        fu_password=TEST_FU_PASSWORD,
        global_unicast_enc_key=TEST_ENCRYPTION_KEY,
        authentication_key=TEST_AUTHENTICATION_KEY
    )

    test_executor.execute_test(meter, verification_nic_set_all_keys,
                               "CREDENTIALS_ALL_KEYS_NIC",
                               set_all_keys_res and connection_nic_us_res)
    test_executor.execute_test(
        meter, verification_pt_set_all_keys, "CREDENTIALS_ALL_KEYS_PT",
        set_all_keys_res and connection_pt_mr_res and connection_pt_us_res and connection_pt_fu_res
    )


def get_meter_device_id(meter) -> Result:
    """ Get meter device id in passthrough. """
    device_id = meter.get_meter_device_ID(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    if device_id and device_id.value:
        logging.info('Device id is %s', device_id)

    return Result(device_id.error_code == ErrorCodeEnum.RES_OK)


def disconnect_reconnect(meter) -> Result:
    """ Disconnect and reconnect a meter and check for a ESW after the disconnect. """
    valid_listen_results = (DataNotificationListenResult.RES_OK,
                            DataNotificationListenResult.RES_ERRONEOUS_EP)
    disconnect_resp = meter.disconnect_meter(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    if disconnect_resp.error_code == ErrorCodeEnum.RES_OK:
        listen_result = listen_notification(meter,
                                            NotificationObisEnum.EVENT_STATUS_WORD_PUSH,
                                            timeout_s=60)

        if listen_result != DataNotificationListenResult.RES_OK:
            message = f"No valid ESW has been reveived after the meter {meter.node_id} was disconnected!"
        elif valid_listen_results == DataNotificationListenResult.RES_ERRONEOUS_EP:
            message = "Erroneous endpoint"
        else:
            message = None

        reconnect_resp = meter.reconnect_meter(connection_type=AssociationLevelEnum.US_ASSOCIATION)
        return Result(reconnect_resp.error_code == ErrorCodeEnum.RES_OK \
                      and listen_result in valid_listen_results,
                      message)

    return Result(False, "Disconnect request had no response!")


def get_meter_esw1(meter) -> Result:
    """ Get meter ESW1 in passthrough. """
    response = meter.get_meter_ESW1(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return Result(response.error_code == ErrorCodeEnum.RES_OK and response.value)


def set_NIC_clock(meter) -> Result:
    """
    Set clock of the meter from the NIC server
    and verify the time is changed in the meter.
    """
    test_result = True

    # Approximate the travel time by supposing the travel time of the packets
    # to the NIC in uplink is the same as the one in downlink.
    get_clock_start_time = time()
    resp_get_clock = meter.get_meter_clock()
    get_approx_travel_time_s = (time() - get_clock_start_time) // 2

    if not resp_get_clock or not resp_get_clock.value:
        return Result(False, "Meter time could not be retrieved!")

    clock_start = meter.client.to_datetime(resp_get_clock.value)
    if not meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        return Result(False)

    # Shift meter clock by 59 minutes with a set clock request to the NIC server.
    time_to_set = clock_start + timedelta(
        seconds=int(time()) - get_clock_start_time + get_approx_travel_time_s + 59*60
    )

    set_clock_start_time = time()
    set_resp = meter.set_NIC_clock(time_to_set)
    set_approx_travel_time_s = (time() - set_clock_start_time) // 2
    if not meter.release_AA_NIC() or set_resp.error_code != ErrorCodeEnum.RES_OK:
        logging.warning("Something wrong happened when setting NIC clock!")

    # Get meter clock and verify it has been changed to the good value.
    get_clock_start_time = time()
    nic_clock = meter.get_meter_clock()
    get_approx_travel_time_s = (time() - get_clock_start_time) // 2
    if not nic_clock.value:
        logging.error("An error occured when getting meter clock after setting it.")
        return Result(False)

    # Verify that the meter clock has shifted.
    min_expected_clock = time_to_set - timedelta(seconds=(set_approx_travel_time_s + 1) * 2)
    max_expected_clock = time_to_set + timedelta(seconds=2*(set_approx_travel_time_s + get_approx_travel_time_s + 2))
    if not min_expected_clock <= meter.client.to_datetime(nic_clock.value) <= max_expected_clock:
        test_result = False
        logging.error("Setting NIC clock made the meter clock diverge! => "
                      "Meter clock is %s but it was expected to be between %s and %s",
                      meter.client.to_datetime(nic_clock.value),
                      min_expected_clock, max_expected_clock)

    logging.info("Put back the meter time to normal.")
    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION) \
            and meter.set_NIC_clock(clock_start + timedelta(seconds=int(time() - get_clock_start_time))) \
            and meter.release_AA_NIC():

        nic_clock = meter.get_meter_clock()
        max_expected_clock = clock_start + timedelta(seconds=time() - get_clock_start_time + meter.response_timeout_s)
        if not clock_start <= meter.client.to_datetime(nic_clock.value) <= max_expected_clock:
            test_result = False
            logging.warning("Time could not be set back, current time is %s, "
                            "but it was expected to be between %s and %s",
                            meter.client.to_datetime(nic_clock.value),
                            clock_start, max_expected_clock)
    else:
        test_result = False

    return Result(test_result)


def set_NIC_instantaneous_interval(meter) -> Result:
    """ Set instantaneous profile push interval of the meter from the NIC. """
    if meter.establish_AA_NIC(AssociationLevelEnum.US_ASSOCIATION):
        get_resp1 = meter.get_NIC_instantaneous_push_interval()
        if get_resp1.error_code != ErrorCodeEnum.RES_OK or not get_resp1.value:
            meter.release_AA_NIC()
            return Result(False, "Instantaneous push interval could not be retrieved from the NIC.")

        # Setting the same interval should not change the push interval.
        set_resp = meter.set_NIC_instantaneous_push_interval(get_resp1.value)
        get_resp2 = meter.get_NIC_instantaneous_push_interval()

        return Result(meter.release_AA_NIC() and set_resp.error_code == ErrorCodeEnum.RES_OK
            and get_resp1.value == get_resp2.value and get_resp1.value is not None)

    return Result(False)


def request_nic_endpoint(meter) -> Result:
    """ Send a simple request to the NIC server with a new destination endpoint
    and verify that the endpoint of the response is the same.
    """
    former_dst_ep = meter.dni.destination_endpoint
    if former_dst_ep != DestinationEndpointEnum.ON_DEMAND_DATA.value[0]:
        new_dst_ep = former_dst_ep - 1
    else:
        new_dst_ep = DestinationEndpointEnum.ON_DEMAND_DATA.value[-1]

    meter.dni.destination_endpoint = new_dst_ep

    # Send a basic request
    if meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        ic_resp = meter.get_NIC_invocation_counter()
        meter.release_AA_NIC()

    # Set back former destination endpoint of the dni
    meter.dni.destination_endpoint = former_dst_ep

    return Result(ic_resp.src_ep.enum == SourceEndpointEnum.ENERGY_METER_ON_DEMAND_DATA
                  and ic_resp.dst_ep.value == new_dst_ep)


def request_pt_endpoint(meter) -> Result:
    """ Send a simple request in passthrough with a new destination endpoint
    and verify that the endpoint of the response is the same.
    """
    former_dst_ep = meter.dni.destination_endpoint
    if former_dst_ep != DestinationEndpointEnum.ON_DEMAND_DATA.value[0]:
        new_dst_ep = former_dst_ep - 1
    else:
        new_dst_ep = DestinationEndpointEnum.ON_DEMAND_DATA.value[-1]

    meter.dni.destination_endpoint = new_dst_ep

    # Send a basic request
    clock_resp = meter.get_meter_clock(AssociationLevelEnum.PC_ASSOCIATION)

    # Set back former destination endpoint of the dni
    meter.dni.destination_endpoint = former_dst_ep

    return Result(clock_resp.src_ep.enum == SourceEndpointEnum.ENERGY_METER_ON_DEMAND_DATA
                  and clock_resp.dst_ep.value == new_dst_ep)


def request_nic_wrong_ep(meter) -> Result:
    """ Send a simple request to the NIC server with a new destination endpoint
    and verify that the endpoint of the response is the same.
    """
    former_dst_ep = meter.dni.destination_endpoint
    new_dst_ep = TEST_WRONG_ENDPOINT
    meter.dni.destination_endpoint = new_dst_ep

    # Send a basic request
    if meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        meter.release_AA_NIC()
        meter.dni.destination_endpoint = former_dst_ep
        return Result(False, "An association has been establushed with a wrong endpoint!")

    # Set back former destination endpoint of the dni
    meter.dni.destination_endpoint = former_dst_ep

    if meter.establish_AA_NIC(AssociationLevelEnum.PC_ASSOCIATION):
        meter.dni.destination_endpoint = TEST_WRONG_ENDPOINT
        ic_resp = meter.get_NIC_invocation_counter()

        meter.dni.destination_endpoint = former_dst_ep
        return Result(meter.release_AA_NIC() and (ic_resp.error_code != ErrorCodeEnum.RES_OK or not ic_resp.value))

    meter.dni.destination_endpoint = former_dst_ep
    return Result(False, "Association could not be established!")


def request_pt_wrong_ep(meter) -> Result:
    """ Send a simple request in passthrough with a new destination endpoint
    and verify that the endpoint of the response is the same.
    """
    former_dst_ep = meter.dni.destination_endpoint
    new_dst_ep = TEST_WRONG_ENDPOINT
    meter.dni.destination_endpoint = new_dst_ep

    # Send a basic request
    clock_resp = meter.get_meter_clock(AssociationLevelEnum.PC_ASSOCIATION)

    # Set back former destination endpoint of the dni
    meter.dni.destination_endpoint = former_dst_ep
    return Result(clock_resp.error_code != ErrorCodeEnum.RES_OK or not clock_resp.value)


def verify_profile_generic_pt(meter, profile_generic: ProfileGeneric, buffer: Response,
                              scaler=None, can_be_empty=False) -> Result:
    """ Verify the buffer, capture objects and scaler of a profile generic in passthrough. """
    profile_generic_name = profile_generic.name.lower().replace("_", " ")
    capture_objects = meter.get_meter_profile_generic_capture_objects(
        profile_generic, connection_type=AssociationLevelEnum.US_ASSOCIATION)

    if scaler is None:
        scaler = meter.get_meter_profile_generic_scaler(
            profile_generic, connection_type=AssociationLevelEnum.US_ASSOCIATION)

    if capture_objects.error_code != ErrorCodeEnum.RES_OK or \
            scaler.error_code != ErrorCodeEnum.RES_OK:
        return Result(False, f"{profile_generic_name} profile information could not be retrieved entirely.")

    if len(scaler.value) > len(capture_objects.value):
        logging.error("%s scaler should be smaller in size than the buffer "
                    "and capture objects, but found: %s and %s",
                    profile_generic_name, len(scaler.value), len(buffer.value[0]))
        return Result(False)

    if not buffer.value:  # If the buffer can be and is empty, the entries in use are checked.
        if not can_be_empty:
            return Result(False, f"No {profile_generic_name} profile buffer has been found.")

        entry_in_uses = meter.get_meter_profile_generic_by_attribute(
            profile_generic.obis_code, attribute_id=7).value

        if entry_in_uses == 0:
            logging.info("No valid %s has been received as the meter does not have "
                         "entries in use for this profile generic.", profile_generic_name)
            return Result(True, "No entry in the meter for the pg")

    if len(buffer.value[0]) != len(capture_objects.value):
        logging.error("%s buffer and capture objects should have the same size, "
                      "but found: %s and %s", profile_generic_name,
                      len(buffer.value[0]), len(capture_objects.value))
        return Result(False)

    return Result(True)


def get_meter_instantaneous_profile(meter) -> Result:
    """ Read instantaneous profile and scaler in passthrough. """
    buffer = meter.get_meter_instantaneous_profile(
        connection_type=AssociationLevelEnum.US_ASSOCIATION, attribute_id=2)
    return verify_profile_generic_pt(meter, ProfileGeneric.INSTANTANEOUS, buffer)


def get_meter_block_load_profile(meter) -> Result:
    """ Read block load profile and scaler in passthrough. """
    buffer = meter.get_meter_block_load_profile(connection_type=AssociationLevelEnum.US_ASSOCIATION,
                                                start=datetime.now() - timedelta(hours=1),
                                                end=datetime.now())
    return verify_profile_generic_pt(meter, ProfileGeneric.BLOCK_LOAD, buffer)


def get_meter_daily_load_profile(meter) -> Result:
    """ Read daily load profile and scaler in passthrough. """
    buffer = meter.get_meter_daily_load_profile(connection_type=AssociationLevelEnum.US_ASSOCIATION,
                                                start=datetime.now() - timedelta(days=2),
                                                end=datetime.now())
    return verify_profile_generic_pt(meter, ProfileGeneric.DAILY_LOAD, buffer)


def get_meter_billing_profile(meter) -> Result:
    """ Read billing profile and scaler in passthrough. """
    buffer = meter.get_meter_billing_profile(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.BILLING, buffer)


def get_meter_name_plate_details(meter) -> Result:
    """ Read name plate details profile in passthrough. """
    buffer = meter.get_meter_name_plate_details(connection_type=AssociationLevelEnum.US_ASSOCIATION)
    capture_objects = meter.get_meter_profile_generic_capture_objects(
        ProfileGeneric.NAME_PLATE, connection_type=AssociationLevelEnum.US_ASSOCIATION)

    if buffer.error_code != ErrorCodeEnum.RES_OK or \
            capture_objects.error_code != ErrorCodeEnum.RES_OK:
        return Result(False, "Name plate details information could not be retrieved entirely.")

    if not buffer.value:
        return Result(False, "No name plate details buffer has been found.")

    if len(buffer.value[0]) != len(capture_objects.value):
        logging.error("Name plate details buffer and capture objects should have the same size, but found: %s and %s",
                      len(buffer.value[0]), len(capture_objects.value))
        return Result(False)

    return Result(True)


def get_meter_voltage_related_events_log(meter, scaler: Response) -> Result:
    """ Read voltage related events log profile in passthrough. """
    buffer = meter.get_meter_voltage_related_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.VOLTAGE_EVENTS_LOG, buffer,
                                     scaler, can_be_empty=True)


def get_meter_current_related_events_log(meter, scaler: Response) -> Result:
    """ Read current related events log profile in passthrough. """
    buffer = meter.get_meter_current_related_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.CURRENT_EVENTS_LOG, buffer,
                                     scaler, can_be_empty=True)


def get_meter_power_related_events_log(meter, scaler: Response) -> Result:
    """ Read power related events log profile in passthrough. """
    buffer = meter.get_meter_power_related_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.POWER_EVENTS_LOG, buffer,
                                     scaler, can_be_empty=True)


def get_meter_transaction_related_events_log(meter, scaler: Response) -> Result:
    """ Read transaction related events log profile in passthrough. """
    buffer = meter.get_meter_transaction_related_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.TRANSACTION_EVENTS_LOG,
                                     buffer, scaler, can_be_empty=True)


def get_meter_other_events_log(meter, scaler: Response) -> Result:
    """ Read other events log profile in passthrough. """
    buffer = meter.get_meter_other_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.OTHER_EVENTS_LOG,
                                     buffer, scaler, can_be_empty=True)


def get_meter_non_rollover_events_log(meter, scaler: Response) -> Result:
    """ Read non-rollover events log profile in passthrough. """
    buffer = meter.get_meter_non_rollover_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.NON_ROLLOVER_EVENTS_LOG,
                                     buffer, scaler, can_be_empty=True)


def get_meter_control_events_log(meter, scaler: Response) -> Result:
    """ Read control events log profile in passthrough. """
    buffer = meter.get_meter_control_events_log(
        index=1, count=1, connection_type=AssociationLevelEnum.US_ASSOCIATION)
    return verify_profile_generic_pt(meter, ProfileGeneric.CONTROL_EVENTS_LOG,
                                     buffer, scaler, can_be_empty=True)


class TestHandler:
    """ Class in charge to handle the tests. """
    def __init__(self, test_tags: List[str], test_tags_to_exclude: List[str]):
        # Verification of the tags
        valid_test_tags = [tag.value for tag in TestTag]
        assert set(test_tags) <= set(valid_test_tags), \
            f"Input test tags should be chosen in the following list: {valid_test_tags}, but found: {test_tags}."
        assert set(test_tags_to_exclude) <= set(valid_test_tags) and "all" not in test_tags_to_exclude, \
            f"Input test tags to exclude should be chosen between {valid_test_tags[1:]}, found: {test_tags_to_exclude}."

        # Conversion of string to test tags as they are all valid.
        test_tags = [TestTag(tag) for tag in test_tags]
        test_tags_to_exclude = [TestTag(tag) for tag in test_tags_to_exclude]

        self.test_tags = test_tags
        if TestTag.EXECUTE_ALL_TESTS in self.test_tags:
            self.test_tags = list(TestTag)
            self.test_tags.remove(TestTag.EXECUTE_ALL_TESTS)

        for tag_to_remove in test_tags_to_exclude:
            if tag_to_remove in self.test_tags:
                self.test_tags.remove(tag_to_remove)

    def get_test_to_execute(self) -> List[TestTag]:
        """ Return the tags of the test to execute. """
        return self.test_tags

    def tag(*fn_tags):
        """ Decorator that adds tags to the test functions so that they can be desactivated. """
        def wrapper(test_fn):
            def wrapper_bis(self, *args, **kwargs):
                """ Function that does nothing to be returned to desactivate tests. """
                for tag in fn_tags:
                    if tag not in self.get_test_to_execute():
                        return

                return test_fn(self, *args, **kwargs)
            return wrapper_bis
        return wrapper

    # Methods to execute group of tests.
    @tag(TestTag.PROVISIONING_TESTS)
    def _test_provisioning(self, node_id):
        """ Execute tests based on the NIC provisioning. """
        status_res = test_executor.execute_test(recreate_meter(node_id), nic_status_word_test,
                                                "PROV_NIC_STATUS_WORD", [nic_status_word_condition])
        test_executor.execute_test(recreate_meter(node_id), provisioning_test,
                                   "PROV_NIC_PROVISIONING", status_res.test_result_bool)

    @tag(TestTag.PROFILE_PUSH_ON_FIRST_CONNECTION_TESTS)
    def _test_profile_push_on_first_connection(self, node_id):
        """ Execute tests based on sending profile generic on first connection. """
        test_executor.execute_test(recreate_meter(node_id), listen_name_plate_on_first_connection,
                                   "FIRST_CONN_NAME_PLATE")

    @tag(TestTag.CONNECTION_TESTS)
    def _test_connection(self, node_id):
        """ Execute connection-related tests. """
        test_executor.execute_test(recreate_meter(node_id), connection_nic_pc, "CONN_NIC_PC")
        test_executor.execute_test(recreate_meter(node_id), connection_nic_mr, "CONN_NIC_MR")
        test_executor.execute_test(recreate_meter(node_id), connection_nic_us, "CONN_NIC_US")
        test_executor.execute_test(recreate_meter(node_id), connection_nic_fu, "CONN_NIC_FU")
        test_executor.execute_test(recreate_meter(node_id), connection_pt_pc, "CONN_PT_PC")
        test_executor.execute_test(recreate_meter(node_id), connection_pt_mr, "CONN_PT_MR")
        test_executor.execute_test(recreate_meter(node_id), connection_pt_us, "CONN_PT_US")
        test_executor.execute_test(recreate_meter(node_id), connection_pt_fu, "CONN_PT_FU")

    @tag(TestTag.NIC_SUPPORTED_OBIS_TESTS)
    def _test_nic_objects_requests(self, node_id):
        """ Execute NIC objects related tests. """
        test_executor.execute_test(recreate_meter(node_id), get_nic_pc_supported_objects,
                                   "NIC_REQ_PC_SUPPORTED_OBJ", [connection_nic_pc])
        test_executor.execute_test(recreate_meter(node_id), get_nic_us_supported_objects,
                                   "NIC_REQ_US_SUPPORTED_OBJ", [connection_nic_us])

    @tag(TestTag.SECURITY_ACCESS_TESTS)
    def _test_security_access(self, node_id):
        """ Execute security accesses related tests. """
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_pc_privilege_escalation,
                                   "SECU_ACCESS_PT_PC_ESCALATION", [connection_pt_pc])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_mr_privilege_escalation,
                                   "SECU_ACCESS_PT_MR_ESCALATION", [connection_pt_mr])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_mr_unsecured_req,
                                   "SECU_ACCESS_PT_MR_UNSECURE", [connection_pt_mr])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_us_unsecured_req,
                                   "SECU_ACCESS_PT_US_UNSECURE", [connection_pt_us])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_fu_privilege_escalation,
                                   "SECU_ACCESS_PT_FU_ESCALATION", [connection_pt_fu])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_fu_unsecured_req,
                                   "SECU_ACCESS_PT_FU_UNSECURE", [connection_pt_fu])
        test_executor.execute_test(recreate_meter(node_id), secu_access_nic_pc_privilege_escalation,
                                   "SECU_ACCESS_NIC_PC_ESCALATION", [connection_nic_pc])
        test_executor.execute_test(recreate_meter(node_id), secu_access_nic_us_unsecured_req,
                                   "SECU_ACCESS_NIC_US_UNSECURE_REQ", [connection_nic_us])
        test_executor.execute_test(recreate_meter(node_id), secu_access_nic_us_unsecured_aa,
                                   "SECU_ACCESS_NIC_US_UNSECURE_AA", [connection_nic_us])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_wrong_invocation_counter,
                                   "SECU_ACCESS_PT_WRONG_IC", [connection_pt_us])
        test_executor.execute_test(recreate_meter(node_id), secu_access_nic_wrong_invocation_counter,
                                   "SECU_ACCESS_NIC_WRONG_IC", [connection_nic_us])
        test_executor.execute_test(recreate_meter(node_id), secu_access_pt_wrong_system_title,
                                   "SECU_ACCESS_PT_WRONG_ST", [connection_pt_us])
        test_executor.execute_test(recreate_meter(node_id), request_nic_increment_ic,
                                   "METER_REQ_NIC_INCREMENT_IC", [connection_pt_us])
        test_executor.execute_test(recreate_meter(node_id), request_pt_increment_ic,
                                   "METER_REQ_PT_INCREMENT_IC", [connection_nic_us])

    @tag(TestTag.CREDENTIALS_TESTS)
    def _test_credentials_change(self, node_id):
        """ Execute credentials-related tests. """
        change_mr_password_tests(recreate_meter(node_id))
        change_us_password_tests(recreate_meter(node_id))
        change_fu_password_tests(recreate_meter(node_id))

        if meter_informations[node_id][SAME_AUTH_ENC_KEYS_TAG]:
            change_enc_auth_key_tests(recreate_meter(node_id))
        else:
            change_encryption_key_tests(recreate_meter(node_id))
            change_authentication_key_tests(recreate_meter(node_id))
            change_all_keys_at_once_tests(recreate_meter(node_id))

    @tag(TestTag.BASIC_OBJ_REQUESTS_TESTS)
    def _test_object_queries(self, node_id):
        """ Execute tests based on meter objects queries. """
        logging.info("Testing condition for most meter object requests tests")
        connection_pt_us_res = connection_pt_us(recreate_meter(node_id)).test_result_bool
        connection_nic_us_res = connection_nic_us(recreate_meter(node_id)).test_result_bool

        test_executor.execute_test(recreate_meter(node_id), get_meter_device_id,
                                   "METER_REQ_PT_GET_DEVICE_ID", connection_pt_us_res)
        test_executor.execute_test(recreate_meter(node_id), disconnect_reconnect,
                                   "METER_REQ_PT_DISCONNECT", connection_pt_us_res)
        test_executor.execute_test(recreate_meter(node_id), get_meter_esw1,
                                   "METER_REQ_PT_GET_ESW1", connection_pt_us_res)
        test_executor.execute_test(recreate_meter(node_id), set_NIC_clock,
                                   "METER_REQ_NIC_SET_CLOCK", connection_nic_us_res and connection_pt_us_res)
        test_executor.execute_test(recreate_meter(node_id), set_NIC_instantaneous_interval,
                                   "METER_REQ_NIC_INSTANT_PERIOD", connection_nic_us_res)
        test_executor.execute_test(recreate_meter(node_id), request_nic_endpoint,
                                   "METER_REQ_NIC_EP", connection_nic_us_res)
        test_executor.execute_test(recreate_meter(node_id), request_pt_endpoint,
                                   "METER_REQ_PT_EP", connection_pt_us_res)
        test_executor.execute_test(recreate_meter(node_id), request_nic_wrong_ep,
                                   "METER_REQ_NIC_WRONG_EP", [connection_pt_pc])
        test_executor.execute_test(recreate_meter(node_id), request_pt_wrong_ep,
                                   "METER_REQ_PT_WRONG_EP", [connection_pt_pc])

    @tag(TestTag.PROFILE_GENERIC_QUERIES)
    def _test_profile_generic_queries(self, node_id):
        """ Get all profile generic to understand better the meter environment. """
        meter = recreate_meter(node_id)
        logging.info("Get all profile generics.")
        test_executor.execute_test(meter, get_meter_instantaneous_profile, "PG_QUERIES_PT_INSTANTANEOUS")
        test_executor.execute_test(meter, get_meter_block_load_profile, "PG_QUERIES_PT_BLOCK_LOAD")
        test_executor.execute_test(meter, get_meter_daily_load_profile, "PG_QUERIES_PT_DAILY_LOAD")
        test_executor.execute_test(meter, get_meter_billing_profile, "PG_QUERIES_PT_BILLING")
        test_executor.execute_test(meter, get_meter_name_plate_details, "PG_QUERIES_PT_NAME_PLATE")

        # Get the scalar that all the events logs profile use.
        events_log_scaler = meter.get_meter_profile_generic_scaler(
            ProfileGeneric.VOLTAGE_EVENTS_LOG, connection_type=AssociationLevelEnum.US_ASSOCIATION)
        test_executor.execute_test(meter, get_meter_voltage_related_events_log,
                                   "PG_QUERIES_PT_VOLTAGE_EVENTS", scaler=events_log_scaler)
        test_executor.execute_test(meter, get_meter_current_related_events_log,
                                   "PG_QUERIES_PT_CURRENT_EVENTS", scaler=events_log_scaler)
        test_executor.execute_test(meter, get_meter_power_related_events_log,
                                   "PG_QUERIES_PT_POWER_EVENTS", scaler=events_log_scaler)
        test_executor.execute_test(meter, get_meter_transaction_related_events_log,
                                   "PG_QUERIES_PT_TRANSACTION_EVENTS", scaler=events_log_scaler)
        test_executor.execute_test(meter, get_meter_other_events_log,
                                   "PG_QUERIES_PT_OTHER_EVENTS", scaler=events_log_scaler)
        test_executor.execute_test(meter, get_meter_non_rollover_events_log,
                                   "PG_QUERIES_PT_NON_ROLLOVER_EVENTS", scaler=events_log_scaler)
        test_executor.execute_test(meter, get_meter_control_events_log,
                                   "PG_QUERIES_PT_CONTROL_EVENTS", scaler=events_log_scaler)


    def run(self, node_id):
        """ Run the tests for a meter.
        It asks the confirmation from the user to execute the tests
        if they include meter credentials changing tests.
        """
        logging.info("The tests concerned by the following tags will be executed: %s",
                     ", ".join([tag.value for tag in self.test_tags]))

        # Ask the confirmation from the user to execute the tests if they include meter credentials changing tests.
        if TestTag.CREDENTIALS_TESTS in self.get_test_to_execute():
            input("Tests that change the meter credentials will be executed! "
                  "Type 'enter' to confirm the test launch.\n")

        logging.info("Execute tests for %s", node_id)
        self._test_provisioning(node_id)
        self._test_profile_push_on_first_connection(node_id)
        self._test_connection(node_id)
        self._test_nic_objects_requests(node_id)
        self._test_security_access(node_id)
        self._test_credentials_change(node_id)
        self._test_object_queries(node_id)
        self._test_profile_generic_queries(node_id)
        logging.info("Tests execution for %s is finished", node_id)


def tags_list(tags: str) -> list:
    return [tag.lower() for tag in tags.split(",")]


if __name__ == "__main__":
    # Check python library version
    INTEGRATION_VERSION = "1.4"
    assert version('wirepas_dlms_tool') == INTEGRATION_VERSION, \
        f"Wirepas DLMS tool Python must be version {INTEGRATION_VERSION}, but found {version('wirepas_dlms_tool')}"

    # Parse user settings.
    parser = argparse.ArgumentParser(formatter_class=RawTextHelpFormatter)
    parser.add_argument(
        "--filename",
        default="integration_tests_output.log",
        type=str,
        help="Name of the file to store the logs of the integration tests.\n"
        "By default: './integration_tests_output.log'",
    )
    parser.add_argument(
        "--test_tags_to_execute",
        required=True,
        type=tags_list,
        help='Tags of the tests that need to be executed separed by ",":\n'
        "Example: --test_tags_to_execute connection,pg_queries\n"
        '"all": All tests that are not present in the tests to exclude.\n'
        '"provisioning": Tests related to the provisioning of the NIC sever.\n'
        '"first_connection_profiles: Tests that listen to the first connection profile pushes.\n'
        '"connection": Tests that verify the connection of the backend to the NIC server and to the meter.\n'
        '"nic_supported_obis": Tests to get supported obis list from the NIC server.\n'
        '"security_access": Security access related tests of the NIC server.\n'
        '"credentials": Tests that change the credentials of the meter.\n'
        '"basic_obj_requests": Tests sending other basic objects queries in passthrough and to the NIC.\n'
        '"pg_queries": Query meter profile generic for environment understandings (No tests execution).'
    )
    parser.add_argument(
        "--test_tags_to_exclude",
        default=[],
        type=tags_list,
        help='Tags of the tests separed by "," that should not be executed:\n'
        "Example: --test_tags_to_exclude credentials\n"
        '"provisioning": Tests related to the provisioning of the NIC sever.\n'
        '"first_connection_profiles: Tests that listen to the first connection profile pushes.\n'
        '"connection": Tests that verify the connection of the backend to the NIC server and to the meter.\n'
        '"nic_supported_obis": Tests to get supported obis list from the NIC server.\n'
        '"security_access": Security access related tests of the NIC server.\n'
        '"credentials": Tests that change the credentials of the meter.\n'
        '"basic_obj_requests": Tests sending other basic objects queries in passthrough and to the NIC.\n'
        '"pg_queries": Query meter profile generic for environment understandings (No tests execution).'
    )

    parser.add_argument(
        "--config",
        default="default_config.json",
        type=str,
        help="Path to the configuration file (e.g., config.json)"
    )
    parse_args = parser.parse_args()

    # Load the configuration
    config = load_config(parse_args.config)

    # Initialize global variables
    initialize_globals(config)

    # Set up the logs.
    logging.basicConfig(
        format='%(asctime)s | [%(levelname)s] %(filename)s:%(lineno)d:%(funcName)s:%(message)s',
        level="DEBUG",
        filename=parse_args.filename)

    print("Information on the DLMS exchanges can be found in the following "
          f"log file: {parse_args.filename}")

    # Prepare the connection to the wirepas network.
    wni = WirepasNetworkInterface(
        host=MQTT_SETTINGS["host"],
        port=MQTT_SETTINGS["port"],
        username=MQTT_SETTINGS["username"],
        password=MQTT_SETTINGS["password"],
        strict_mode=False,
    )

    # Interface to handle nodes update on network and to distribute the receveived message to the requested meter object.
    dni = DLMSNetworkInterface(wni, nodes=list(METER_CONFIGURATIONS.keys()))

    # Tests executor to execute test and to show the results.
    test_executor = TestExecutor()

    # Meter former settings to be retrieved at the end of the tests.
    meter_informations: Dict[int, Dict[str, Any]] = {}

    # Create the test handler.
    test_handler = TestHandler(parse_args.test_tags_to_execute, parse_args.test_tags_to_exclude)
    test_threads = []

    # Run all meters tests in parallel.
    for node, meter_config in METER_CONFIGURATIONS.items():
        meter_informations[node] = {
            CONFIGURATION_TAG: deepcopy(meter_config[CONFIGURATION_TAG]),
            NIC_SYSTEM_TITLE_TAG: meter_config.get(NIC_SYSTEM_TITLE_TAG, None),
            METER_SERIAL_NUMBER_TAG: meter_config.get(METER_SERIAL_NUMBER_TAG, None),
            METER_IC_TAG: meter_config.get(METER_IC_TAG, 1),
            SAME_AUTH_ENC_KEYS_TAG: meter_config.get(SAME_AUTH_ENC_KEYS_TAG, True)
        }

        thread = Thread(target=test_handler.run, args=[node], daemon=True)
        test_threads.append(thread)
        thread.start()

    # Make sure all tests are finished.
    for thread in test_threads:
        thread.join()

    # Print test results.
    logging.info("Test results:")
    test_executor.print_all_test_results()
