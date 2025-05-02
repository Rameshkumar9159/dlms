# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#

import argparse
from colorama import Fore
import logging
import queue
import serial
import sys
import time
import yahdlc

from time import sleep
from threading import Thread, Event
from struct import pack, unpack
from enum import IntEnum

from yahdlc import (
    FRAME_ACK,
    FRAME_DATA,
    FRAME_NACK,
    FCSError,
    MessageError,
    frame_data,
    get_data,
    get_data_reset,
)


class ExitCodes(IntEnum):
    SUCCESS = 0
    SENDING_FAILED = 1
    RECEPTION_TIMEOUT = 2
    INVALID_PARAMETERS = 3
    TEST_FAILED = 4
    TEST_ALREADY_DONE = 5
    SERIAL_ERROR = 6


class MessageType(IntEnum):
    REQ_INIT_DUT = 0
    RSP_INIT_DUT = 1
    REQ_TEST_EXT_FLASH = 2
    RSP_TEST_EXT_FLASH = 3
    REQ_TEST_RF = 4
    RSP_TEST_RF = 5
    REQ_CONFIG_ROUTER = 240
    RSP_CONFIG_ROUTER = 241


KEY_SIZE = 16
MINIMUM_NETWORK_ADDRESS = 1
MAXIMUM_NETWORK_ADDRESS = 0xFFFFFF
MINIMUM_NETWORK_CHANNEL = 1
MAXIMUM_NETWORK_CHANNEL = 12
MINIMUM_NODE_ADDRESS = 1
# 0x0xFFFFFFFE => APP_ADDR_ANYSINK
# 0x0xFFFFFFFF => APP_ADDR_BROADCAST
MAXIMUM_NODE_ADDRESS = 0xFFFFFFFD


class Communication:
    SERIAL_BAUDRATE = 115200
    ACK_NACK_TIMEOUT_S = 0.500
    MAX_TX_ATTEMPT = 3
    TIMEOUT_RSP_CONFIG_ROUTER_S = 3
    EMBEDDED_HDLC_BUFFER_MAX_SIZE = 512

    TIME_FOR_ROUTER_TO_APPLY_PARAMETERS_S = 2

    ser = serial.Serial()

    hdlc_decode = Event()
    tx_seq = 0

    tx_queue = queue.Queue(MAX_TX_ATTEMPT)

    rx_ftype_queue = queue.Queue()
    rsp_config_router_queue = queue.Queue()
    rsp_test_ext_flash_queue = queue.Queue()
    rsp_test_rf_queue = queue.Queue()


def success_print(message):
    logging.info(Fore.GREEN + message + Fore.RESET)


def print_summary_error_and_exit(message: str, error_code: ExitCodes):
    logging.error(Fore.RED + message + Fore.RESET)
    exit(error_code.value)


def validate_key(key: str) -> bytes:
    byte_data = bytes.fromhex(key)
    if len(byte_data) != KEY_SIZE:
        raise argparse.ArgumentTypeError(
            "Given key must be " + str(KEY_SIZE) + " bytes long"
        )
    return byte_data


def validate_network_address(network_address: str) -> int:
    network_address = int(network_address, 0)
    if network_address not in range(
        MINIMUM_NETWORK_ADDRESS, MAXIMUM_NETWORK_ADDRESS + 1
    ):
        raise argparse.ArgumentTypeError("Given Network Address is out of range")
    return network_address


def validate_network_channel(network_channel: str) -> int:
    network_channel = int(network_channel, 0)
    if network_channel not in range(
        MINIMUM_NETWORK_CHANNEL, MAXIMUM_NETWORK_CHANNEL + 1
    ):
        raise argparse.ArgumentTypeError("Given Network Channel is out of range")
    return network_channel


def validate_node_address(node_address: str) -> int:
    node_address = int(node_address, 0)
    if node_address < MINIMUM_NODE_ADDRESS or node_address > MAXIMUM_NODE_ADDRESS:
        raise argparse.ArgumentTypeError("Given Node Address is out of range")

    # Checking we are not in the multicast space
    if (node_address & 0xFF000000) == 0x80000000:
        raise argparse.ArgumentTypeError("Given Node Address is in the multicast address range")

    return node_address


def serial_initialization(serial_port: str, com: Communication):
    logging.info(
        "Connecting to " + serial_port + " at " + str(com.SERIAL_BAUDRATE) + " bauds"
    )
    com.ser = serial.Serial(serial_port, com.SERIAL_BAUDRATE)

    rx_thread = Thread(
        target=rx_callback,
        daemon=True,
        args=(com,),
    )
    rx_thread.start()


def send_data(com: Communication, data: bytes, type: MessageType) -> bool:
    # Generating header
    header = pack("<BB", 0, type.value)
    message = header + data
    hdlc_frame = frame_data(message, FRAME_DATA, com.tx_seq)

    logging.debug("Sending " + type.name)
    logging.debug("Payload " + str(data))

    if len(hdlc_frame) > com.EMBEDDED_HDLC_BUFFER_MAX_SIZE:
        print_summary_error_and_exit(
            "Length of the HDLC packet cannot be processed by the embedded application!",
            ExitCodes.HDLC_PACKET_TOO_LARGE,
        )

    for _ in range(com.MAX_TX_ATTEMPT):
        com.tx_queue.put_nowait(hdlc_frame)

    # Cleaning any remaining items stored in the rx_ftype queue
    with com.rx_ftype_queue.mutex:
        com.rx_ftype_queue.queue.clear()

    ret = False
    try:
        while com.tx_queue.empty() is False:
            packet = com.tx_queue.get()
            logging.debug(
                "Sending %u bytes long data frame: %s with seq: %u",
                len(packet),
                packet.hex(),
                com.tx_seq,
            )
            com.ser.write(packet)
            try:
                ftype = com.rx_ftype_queue.get(timeout=com.ACK_NACK_TIMEOUT_S)
            except queue.Empty:
                logging.warning("Timeout detected!")
                continue

            if ftype != FRAME_ACK:
                ret = False
                continue
            else:
                with com.tx_queue.mutex:
                    com.tx_queue.queue.clear()
                ret = True

        com.tx_seq += 1
        com.tx_seq %= 7

        return ret

    except serial.SerialException as err:
        msg = "Serial connection problem:" + err
        print_summary_error_and_exit(msg, ExitCodes.SERIAL_ERROR)


def parse_message_received(com: Communication, data: bytes):
    # parse header
    version, type = unpack("<BB", data[0:2])
    if version != 0:
        logging.warning("Not a valid protocol version")
        return

    # Convert to enum
    type = MessageType(type)

    # Remove header from received data
    data = data[2:]

    if type == MessageType.RSP_CONFIG_ROUTER:
        logging.debug("Received RSP_CONFIG_ROUTER")
        com.rsp_config_router_queue.put_nowait(data)
    else:
        logging.warning("Unknown type received %u", type)


def rx_callback(com: Communication):
    read = bytes()
    while True:
        read += com.ser.read(1)
        try:
            get_data_reset()
            data, ftype, seq_no = get_data(read)
            # logging.debug("RX_Callback %u %s %u", ftype, data.hex(), seq_no)
            com.rx_ftype_queue.put(ftype)
            if ftype == FRAME_ACK:
                logging.debug("Ack for seq: %u", seq_no)
            elif ftype == FRAME_NACK:
                logging.debug("Nack for seq: %u", seq_no)
            elif ftype == FRAME_DATA:
                logging.debug(
                    "Message received: %s (ftype=%u) (seq_no=%u)",
                    data.hex(),
                    ftype,
                    seq_no,
                )
                # Send a ack
                com.ser.write(frame_data("", FRAME_ACK, seq_no))
                parse_message_received(com, data)
            else:
                logging.debug("%s", read.hex())
                logging.debug("Invalid frame", ftype)
            read = bytes()
        except MessageError:
            # No HDLC frame detected.
            pass
        except FCSError:
            logging.warning("Bad FCS")
            pass


def configure_test_router(args: argparse, com: Communication):
    logging.info("Configuring Router")
    req_config_router_msg = pack(
        "<LLB16s16s",
        args.test_router_address,
        args.network_address,
        args.network_channel,
        args.encryption_key,
        args.authentication_key,
    )

    while send_data(com, req_config_router_msg, MessageType.REQ_CONFIG_ROUTER) is False:
        logging.warning(
            Fore.YELLOW + "Please connect the Router to the Test Station" + Fore.RESET
        )

    try:
        config_router_data = com.rsp_config_router_queue.get(
            timeout=com.TIMEOUT_RSP_CONFIG_ROUTER_S
        )
    except queue.Empty:
        print_summary_error_and_exit(
            "RSP_CONFIG_ROUTER was not received in the given timeout!",
            ExitCodes.RECEPTION_TIMEOUT,
        )

    (config_status,) = unpack("<?", config_router_data)

    if config_status is False:
        print_summary_error_and_exit(
            "Invalid parameters given, unable to configure Test Router!",
            ExitCodes.INVALID_PARAMETERS,
        )

    # Waiting for the Test Router to stop the stack and apply the configuration and reboot
    sleep(com.TIME_FOR_ROUTER_TO_APPLY_PARAMETERS_S)
    success_print("✅✅✅ Test Router Successfully Configured!")


if __name__ == "__main__":
    logging.basicConfig(
        format="%(levelname)s %(asctime)s %(message)s", level=logging.INFO
    )

    parser = argparse.ArgumentParser(fromfile_prefix_chars="@")

    # Serial parameters
    parser.add_argument(
        "--serial_port",
        type=str,
        help="Serial port used to communicate with the Test Router.",
        required=True,
    )

    # Testing Router parameters
    parser.add_argument(
        "--test_router_address",
        type=validate_node_address,
        help="Test Router Address.",
        required=True,
    )
    parser.add_argument(
        "--network_address",
        type=validate_network_address,
        help="Network address used by the NIC during the RF testing.",
        required=True,
    )
    parser.add_argument(
        "--network_channel",
        type=validate_network_channel,
        help="Network channel used by the NIC during the RF testing.",
        required=True,
    )
    parser.add_argument(
        "--encryption_key",
        type=validate_key,
        help="Encryption Key used by the NIC during the RF testing. Must be specified as a 16 bytes value.",
        required=True,
    )
    parser.add_argument(
        "--authentication_key",
        type=validate_key,
        help="Authentication Key used by the NIC during the RF testing. Must be specified as a 16 bytes value.",
        required=True,
    )

    args = parser.parse_args()

    logging.debug(args)

    com = Communication()
    serial_initialization(args.serial_port, com)

    configure_test_router(args, com)

    exit(ExitCodes.SUCCESS)
