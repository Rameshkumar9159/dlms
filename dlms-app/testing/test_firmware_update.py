# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging
import time
import datetime
import os
import struct

from wirepas_mqtt_library import WirepasNetworkInterface
from wirepas_dlms_tool import AssociationLevelEnum, DLMSNetworkInterface, ErrorCodeEnum, MeterConfiguration, WirepasNotification
import wirepas_mesh_messaging as wmm
from wirepas_dlms_tool import NotificationObisEnum  # <= Import

METER_FIRMWARE_UPDATE_EP: int = 64
MAX_IMAGE_IDENTIFIER_LEN: int = 32

# TODO: update the node informations.
NODE_ID: int = <node_id>
GATEWAY_ID: str = ""
SINK_ID: str = ""
IMAGE_ID bytes = b""
mfu_completed: bool = False

# TODO: Complete your MQTT settings to connect to the MQTT broker.
MQTT_HOST: str = ""
MQTT_PORT: int = <mqtt_port>
MQTT_USERNAME: str = ""
MQTT_PASSWORD: str = ""

# Network id to filter the messages from the meters.
WIREPAS_NETWORK_FILTER: int = None

def print_log(l):
    print(datetime.datetime.now(tz=None).strftime("%y/%m/%d %H:%M:%S.%f")[:-3], end="", flush=True)
    print(" ", end="", flush=True)
    print(l, flush=True)

MFU_CMD_RESET                           = 0
MFU_CMD_QUERY_FIRMWARE_UPDATE_STATUS    = 1
MFU_CMD_PROCESS_FIRMWARE                = 2

cmd2str = [
    "RESET",
    "QUERY_FIRMWARE_UPDATE_STATUS",
    "PROCESS_FIRMWARE"
]

MFU_CMD_STATUS_ACCEPTED                 = 0
MFU_CMD_STATUS_EXECUTED                 = 1
MFU_CMD_STATUS_EXECUTED_WITH_ERRORS     = 2
TEST_STATUS_INVALID_OR_MISSING_ARGUMENT = 3
MFU_CMD_STATUS_NOT_IMPLEMENTED          = 4
MFU_CMD_STATUS_NOT_SUPPORTED            = 5

status2str = [
    "CMD_ACCEPTED",
    "CMD_EXECUTED",
    "CMD_EXECUTED_WITH_ERRORS",
    "INVALID_OR_MISSING_ARGUMENT",
    "CMD_NOT_IMPLEMENTED",
    "CMD_NOT_SUPPORTED"
]

FIRMWARE_UPDATE_STEP_NO_OP                                  = 0
FIRMWARE_UPDATE_STEP_CHECK_AVAILABILITY                     = 1
FIRMWARE_UPDATE_STEP_READ_UPDATE                            = 2
FIRMWARE_UPDATE_STEP_CONNECT                                = 3
FIRMWARE_UPDATE_STEP_CHECK_IF_IMAGE_TRANSFER_IS_ENABLED     = 4
FIRMWARE_UPDATE_STEP_READ_BLOCK_SIZE                        = 5
FIRMWARE_UPDATE_STEP_CHECK_IMAGE_TRANSFER_STATUS            = 6
FIRMWARE_UPDATE_STEP_INITIATE_TRANSFER                      = 7
FIRMWARE_UPDATE_STEP_TRANSFER_BLOCK                         = 8
FIRMWARE_UPDATE_STEP_VERIFY_IMAGE                           = 9
FIRMWARE_UPDATE_STEP_ACTIVATE_IMAGE                         = 10
FIRMWARE_UPDATE_STEP_RECONNECT                              = 11


mfu_step2str = [
    "NO_OP",
    "CHECK_AVAILABILITY",
    "READ_UPDATE",
    "CONNECT",
    "CHECK_IF_IMAGE_TRANSFER_IS_ENABLED",
    "READ_BLOCK_SIZE",
    "CHECK_IMAGE_TRANSFER_STATUS",
    "INITIATE_TRANSFER",
    "TRANSFER_BLOCK",
    "VERIFY_IMAGE",
    "ACTIVATE_IMAGE",
    "RECONNECT"
]

FIRMWARE_UPDATE_RESULT_OK                               = 0
FIRMWARE_UPDATE_RESULT_ONGOING                          = 1
FIRMWARE_UPDATE_RESULT_NO_AVAILABLE_UPDATE              = 2
FIRMWARE_UPDATE_RESULT_ALREADY_PROCESSED                = 3
FIRMWARE_UPDATE_RESULT_UPDATE_AVAILABLE                 = 4
FIRMWARE_UPDATE_RESULT_ENABLED                          = 5
FIRMWARE_UPDATE_RESULT_DISABLED                         = 6
FIRMWARE_UPDATE_RESULT_INTERNAL_ERROR                   = 7
FIRMWARE_UPDATE_RESULT_CONNECTION_ERROR                 = 8
FIRMWARE_UPDATE_RESULT_LIB_ERROR                        = 9
FIRMWARE_UPDATE_RESULT_METER_ERROR                      = 10
FIRMWARE_UPDATE_RESULT_OPERATION_FAILED                 = 11
FIRMWARE_UPDATE_RESULT_INVALID_OPERATION                = 12
FIRMWARE_UPDATE_RESULT_TEMPORARY_FAILURE                = 13

mfu_status2str = [
    "OK",
    "ONGOING",
    "NO_AVAILABLE_UPDATE",
    "ALREADY_PROCESSED",
    "UPDATE_AVAILABLE",
    "ENABLED",
    "DISABLED",
    "INTERNAL_ERROR",
    "CONNECTION_ERROR",
    "LIB_ERROR",
    "METER_ERROR",
    "OPERATION_FAILED",
    "INVALID_OPERATION",
    "TEMPORARY_FAILURE"
]

def send_data(payload, cmd):

    try:
        res = wni.send_message(GATEWAY_ID, SINK_ID, NODE_ID, METER_FIRMWARE_UPDATE_EP, METER_FIRMWARE_UPDATE_EP, payload)

        if res != wmm.GatewayResultCode.GW_RES_OK:
            print_log(f"⚠ Cannot send data to {GATEWAY_ID}:{SINK_ID} res={res}")

    except TimeoutError:
        print_log(f"⚠ Cannot send data to {GATEWAY_ID}:{SINK_ID}")

def reset():

    print_log("Sending RESET command");
    payload = struct.pack('<B', MFU_CMD_RESET)
    send_data(payload, MFU_CMD_RESET)

def query_firmware_update_status():

    print_log("Sending QUERY_FIRMWARE_UPDATE_STATUS command");
    payload = struct.pack('<B', MFU_CMD_QUERY_FIRMWARE_UPDATE_STATUS)
    send_data(payload, MFU_CMD_QUERY_FIRMWARE_UPDATE_STATUS)

def process_firmware_update():

    #~ #define MAX_IMAGE_IDENTIFIER_LEN    32
    #~ typedef struct
    #~ {
        #~ uint8_t len;
        #~ uint8_t id[MAX_IMAGE_IDENTIFIER_LEN];
    #~ } mfu_command_img_id_t;

    print_log("Sending PROCESS_FIRMWARE command");
    if not len(IMAGE_ID) or len(IMAGE_ID) > MAX_IMAGE_IDENTIFIER_LEN:
        print_log("Error: invalid IMAGE_ID, command will not be sent")
    else:
        payload = struct.pack('<BB%ds' % (len(IMAGE_ID),), MFU_CMD_PROCESS_FIRMWARE, len(IMAGE_ID), IMAGE_ID)
        send_data(payload, MFU_CMD_PROCESS_FIRMWARE)

def decode_cmd(cmd):

    if cmd < len(cmd2str):
        return cmd2str[cmd]

    return f"unknwon command {cmd}"

def decode_status(status):

    if status < len(status2str):
        return status2str[status]

    return f"unknwon status {status}"

def decode_mfu_step(step):

    if step < len(mfu_step2str):
        return mfu_step2str[step]

    return f"unknwon MFU step {step}"

def decode_mfu_status(status):

    if status < len(mfu_status2str):
        return mfu_status2str[status]

    return f"unknwon MFU status {status}"


def on_mfu_traffic_received(data):

    global mfu_completed

    if data.source_address != NODE_ID:
        # Discard traffic from other nodes
        return

    #~ print_log("Received message from {n}".format(n = data.source_address))
    if not data.data_payload or len(data.data_payload) < 3:
        print_log("Invalid data")
        return

    try:
        cmd, status = struct.unpack('<BB', data.data_payload[0:2])
    except struct.error as e:
        print_log(f"Error while unpacking: {str(e)}")
        return

    if cmd == MFU_CMD_PROCESS_FIRMWARE or cmd == MFU_CMD_QUERY_FIRMWARE_UPDATE_STATUS:
        mfu_step , mfu_status =  struct.unpack('<BB', data.data_payload[2:4])
        e = "⚠"
        if status == MFU_CMD_STATUS_ACCEPTED or status == MFU_CMD_STATUS_EXECUTED:
            e = "✅"

        print_log("{e} Command: {c}, status: {s}, MFU step: {step}, MFU status: {status}".format(e = e, c = decode_cmd(cmd), s = decode_status(status), step = decode_mfu_step(mfu_step), status = decode_mfu_status(mfu_status)))
        if cmd == MFU_CMD_PROCESS_FIRMWARE:
            mfu_completed = True

if __name__ == "__main__":
    meter = None

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

    wni.register_uplink_traffic_cb(on_mfu_traffic_received, gateway=GATEWAY_ID, src_ep=METER_FIRMWARE_UPDATE_EP, dst_ep=METER_FIRMWARE_UPDATE_EP)

    # Reset board
    #~ reset()
    #~ time.sleep(60)

    # Query firmware update status
    query_firmware_update_status()
    time.sleep(5)

    # Process firmware
    process_firmware_update()

    while not mfu_completed:
        time.sleep(30)
        query_firmware_update_status()

    time.sleep(5)
    print_log("End of tests")
