# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging
import time
import datetime

from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import AssociationLevelEnum, DLMSNetworkInterface, ErrorCodeEnum, MeterConfiguration
from gurux_dlms.enums import RequestTypes, DataType

# TODO: update the node informations.
NODE_ID: int = <node_id>
GATEWAY_ID: str = ""
SINK_ID: str = ""
NIC_SYSTEM_TITLE: bytes = bytes.fromhex("")

# TODO: Complete your MQTT settings to connect to the MQTT broker.
MQTT_HOST: str = ""
MQTT_PORT: int = <mqtt_port>
MQTT_USERNAME: str = ""
MQTT_PASSWORD: str = ""

# Network id to filter the messages from the meters.
WIREPAS_NETWORK_FILTER: int = None

# TODO: Meter configuration of the meter to query.
# Note: If some keys are not needed, they can be removed to the configuration or left to None.
METER_CONFIGURATION = MeterConfiguration(
    key_encryption_key=None,
    authentication_key=b"".hex(),
    block_cipher_key=b"".hex(),
    mr_password=b"".hex(),
    us_password=b"".hex(),
    fu_password=None
)

def print_log(l):
    print(datetime.datetime.now(tz=None).strftime("%y/%m/%d %H:%M:%S.%f")[:-3], end="")
    print(" ", end="")
    print(l)

def read_nic_server_invocation_counter():
    ic = 0
    print_log("NIC server: establish AA (PC)")
    meter.establish_AA_NIC(connection_type = AssociationLevelEnum.PC_ASSOCIATION)

    print_log("NIC server: read invocation counter (PC)")
    response = meter.get_NIC_data(obis_code="0.0.43.1.3.255")
    if response.error_code == ErrorCodeEnum.RES_OK:
        ic = int(response.value)
        print_log(f"Meter IC: {ic}")
    else:
        print_log(response.error_code)

    print_log("NIC server: release AA (PC)")
    meter.release_AA_NIC()

    return ic


if __name__ == "__main__":
    # Set up the logs
    logging.basicConfig(
        filename="example_request.log",
        format='%(asctime)s | [%(levelname)s] %(filename)s:%(lineno)d:%(funcName)s:%(message)s',
        level="DEBUG")

    # Prepare the connection to the wirepas network.
    wni = WirepasNetworkInterface(MQTT_HOST, MQTT_PORT, MQTT_USERNAME,
                                  MQTT_PASSWORD, strict_mode=False)

    # Our network interface to communication with meters.
    dni = DLMSNetworkInterface(wni, nodes=[NODE_ID], network=WIREPAS_NETWORK_FILTER)

    # Creation of a meter object.
    meter = dni.create_meter(node_id=NODE_ID,
                             meter_configuration=METER_CONFIGURATION,
                             nic_system_title=NIC_SYSTEM_TITLE,
                             gateway=GATEWAY_ID,
                             sink=SINK_ID,
                             response_timeout_s=30)

    # +++
    ic = read_nic_server_invocation_counter()
    print_log("Set meter IC to {n}".format(n = ic))
    meter.invocation_counter = ic

    # +++
    print_log("NIC server: establish AA (US)")
    meter.establish_AA_NIC()

    print_log("NIC server: set registration status (US)")
    response = meter.set_NIC_data(obis_code = "0.0.96.0.1.255", value_to_set = True, attribute_type=DataType.BOOLEAN)

    # Verification of the correctness of the response with the error code.
    if response.error_code == ErrorCodeEnum.RES_OK:
        print_log(response.value)
    else:
        print_log(response.error_code)

    print_log("NIC server: release AA (US)")
    meter.release_AA_NIC()
    time.sleep(5);
