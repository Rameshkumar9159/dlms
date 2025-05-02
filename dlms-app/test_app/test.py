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
    HDLC_PACKET_TOO_LARGE = 7


class MessageType(IntEnum):
    REQ_INIT_DUT = 0
    RSP_INIT_DUT = 1
    REQ_TEST_EXT_FLASH = 2
    RSP_TEST_EXT_FLASH = 3
    REQ_TEST_RF = 4
    RSP_TEST_RF = 5
    REQ_CONFIG_ROUTER = 240
    RSP_CONFIG_ROUTER = 241


class TestInitStatus(IntEnum):
    OK = 0
    CONFIG_ERROR = 1
    TESTING_ALREADY_DONE = 2


KEY_SIZE = 16
MINIMUM_NETWORK_ADDRESS = 1
MAXIMUM_NETWORK_ADDRESS = 0xFFFFFF
MINIMUM_NETWORK_CHANNEL = 1
MAXIMUM_NETWORK_CHANNEL = 12
MINIMUM_NODE_ADDRESS = 1
# 0x0xFFFFFFFE => APP_ADDR_ANYSINK
# 0x0xFFFFFFFF => APP_ADDR_BROADCAST
MAXIMUM_NODE_ADDRESS = 0xFFFFFFFD

NIC_SERIAL_NUMBER_MAX_LENGTH = 32
RX_RSSI_THRESHOLD_DBM = -75
TX_RSSI_THRESHOLD_DBM = -75


class Communication:
    SERIAL_BAUDRATE = 115200
    ACK_NACK_TIMEOUT_S = 0.500
    MAX_TX_ATTEMPT = 3
    TIMEOUT_RSP_INIT_DUT_S = 3
    TIMEOUT_RSP_TEST_EXT_FLASH_S = 10
    TIMEOUT_RSP_TEST_RF_S = 35
    EMBEDDED_HDLC_BUFFER_MAX_SIZE = 512

    ser = serial.Serial()

    hdlc_decode = Event()
    tx_seq = 0

    tx_queue = queue.Queue(MAX_TX_ATTEMPT)

    rx_ftype_queue = queue.Queue()
    rsp_init_dut_queue = queue.Queue()
    rsp_test_ext_flash_queue = queue.Queue()
    rsp_test_rf_queue = queue.Queue()


class TestStatus(IntEnum):
    SUCCESS = 0
    UNTESTED = 1
    FAILED = 2


class TestSummary:
    def __init__(self, test_name, status, duration):
        self.test_name = test_name
        self.status = status
        self.duration = duration

    def set_status(self, status: TestStatus):
        self.status = status

    test_name = ""
    status = TestStatus.UNTESTED
    duration = 0


extflash_summary = TestSummary("💾 External Flash", TestStatus.UNTESTED, 0)
rf_summary = TestSummary("📶 RF", TestStatus.UNTESTED, 0)

nic_serial_number = ""
script_launch_time = time.time()


def success_print(message):
    logging.info(Fore.GREEN + message + Fore.RESET)


def print_summary_error_and_exit(message: str, error_code: ExitCodes):
    logging.error(Fore.RED + message + Fore.RESET)
    print_testing_summary(extflash_summary, rf_summary, script_launch_time)
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


def validate_serial_number(serial_number: str) -> bytes:
    sno = bytes(serial_number, "utf-8")
    if len(sno) > 32:
        raise argparse.ArgumentTypeError("Given NIC Serial Number is too long")

    return sno


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
        msg = "Serial connection problem:" + str(err)
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

    if type == MessageType.RSP_INIT_DUT:
        logging.debug("Received RSP_INIT_DUT")
        com.rsp_init_dut_queue.put_nowait(data)
    elif type == MessageType.RSP_TEST_EXT_FLASH:
        logging.debug("Received RSP_TEST_EXT_FLASH")
        com.rsp_test_ext_flash_queue.put_nowait(data)
    elif type == MessageType.RSP_TEST_RF:
        logging.debug("Received RSP_TEST_RF")
        com.rsp_test_rf_queue.put_nowait(data)
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
                logging.debug("Invalid frame %s", ftype)
            read = bytes()
        except MessageError:
            # No HDLC frame detected.
            pass
        except FCSError:
            logging.warning("Bad FCS")
            pass


def initialize_dut(args: argparse, com: Communication):
    logging.info("Initializing DUT")
    network_settings = pack(
        "<LLB16s16s",
        args.test_router_address,
        args.network_address,
        args.network_channel,
        args.encryption_key,
        args.authentication_key,
    )
    rf_testing_parameters = pack("bb", RX_RSSI_THRESHOLD_DBM, TX_RSSI_THRESHOLD_DBM)
    nic_serial_number = pack("32s", args.serial_number)
    req_init_dut_msg = network_settings + rf_testing_parameters + nic_serial_number

    while send_data(com, req_init_dut_msg, MessageType.REQ_INIT_DUT) is False:
        logging.warning(
            Fore.YELLOW + "Please connect the NIC to the Test Station" + Fore.RESET
        )

    try:
        init_test_data = com.rsp_init_dut_queue.get(timeout=com.TIMEOUT_RSP_INIT_DUT_S)
    except queue.Empty:
        print_summary_error_and_exit(
            "RSP_INIT_DUT was not received in the given timeout!",
            ExitCodes.RECEPTION_TIMEOUT,
        )

    test_init_status, extflash_test_status, rf_test_status = unpack(
        "<BBB", init_test_data
    )
    extflash_summary.set_status(extflash_test_status)
    rf_summary.set_status(rf_test_status)

    if test_init_status == TestInitStatus.CONFIG_ERROR:
        print_summary_error_and_exit(
            "Invalid parameters given, unable to initialize the NIC!",
            ExitCodes.INVALID_PARAMETERS,
        )
    elif test_init_status == TestInitStatus.TESTING_ALREADY_DONE:
        print_summary_error_and_exit(
            "Testing has already been performed on this NIC!",
            ExitCodes.TEST_ALREADY_DONE,
        )


def perform_external_flash_testing(com: Communication, sum: TestSummary) -> bool:
    start = time.time()
    logging.info("💾 Starting External Flash Testing 💾")
    if send_data(com, bytes(), MessageType.REQ_TEST_EXT_FLASH) is False:
        print_summary_error_and_exit(
            "Sending REQ_TEST_EXT_FLASH isn't successful!", ExitCodes.SENDING_FAILED
        )

    try:
        ext_flash_test_data = com.rsp_test_ext_flash_queue.get(
            timeout=com.TIMEOUT_RSP_TEST_EXT_FLASH_S
        )
    except queue.Empty:
        sum.duration = time.time() - start
        sum.status = TestStatus.FAILED
        print_summary_error_and_exit(
            "RSP_TEST_EXT_FLASH was not received in the given timeout!",
            ExitCodes.RECEPTION_TIMEOUT,
        )

    sum.duration = time.time() - start

    (extflash_status,) = unpack("<B", ext_flash_test_data)
    sum.set_status(extflash_status)

    if extflash_status != TestStatus.SUCCESS:
        print_summary_error_and_exit(
            "❌ External Flash is not OK ❌", ExitCodes.TEST_FAILED
        )
    else:
        logging.info("✅ External Flash is OK ✅")

    return sum


def perform_rf_testing(com: Communication, sum: TestSummary) -> bool:
    start = time.time()
    logging.info("📶 Starting RF Testing 📶")
    if send_data(com, bytes(), MessageType.REQ_TEST_RF) is False:
        print_summary_error_and_exit(
            "Sending REQ_TEST_RF isn't successful!", ExitCodes.SENDING_FAILED
        )

    try:
        rf_test_data = com.rsp_test_rf_queue.get(timeout=com.TIMEOUT_RSP_TEST_RF_S)
    except queue.Empty:
        sum.duration = time.time() - start
        sum.status = TestStatus.FAILED
        print_summary_error_and_exit(
            "RSP_TEST_EXT_FLASH was not received in the given timeout!",
            ExitCodes.RECEPTION_TIMEOUT,
        )

    sum.duration = time.time() - start

    rx_rssi, tx_rssi, sum.status = unpack("<bbB", rf_test_data)

    rx_tx_message = "\n\t\t\t\t├ RX RSSI: %d dBm\n\t\t\t\t└ TX RSSI: %d dBm" % (
        rx_rssi,
        tx_rssi,
    )
    if sum.status != TestStatus.SUCCESS:
        message = "❌ RF is not OK ❌" + rx_tx_message
        print_summary_error_and_exit(message, ExitCodes.TEST_FAILED)
    else:
        message = "✅ RF is OK ✅" + rx_tx_message
        logging.info(message)

    return sum


def build_individual_test_summary(sum: TestSummary) -> str:
    sum_print = ""
    if sum.status == TestStatus.SUCCESS:
        sum_print += (
            Fore.GREEN
            + "✅✅✅ "
            + sum.test_name
            + " is OK "
            + ("\t(Took %.3f s)" % sum.duration)
        )
    elif sum.status == TestStatus.FAILED:
        sum_print += (
            Fore.RED
            + "❌❌❌ "
            + sum.test_name
            + " is NOT OK "
            + ("\t(Took %.3f s)" % sum.duration)
        )
    else:
        sum_print += Fore.YELLOW + "❔❔❔ " + sum.test_name + " is Untested "

    sum_print += Fore.RESET
    sum_print += "\n"

    return sum_print


def print_testing_summary(
    extflash_summary: TestSummary,
    rf_summary: TestSummary,
    script_start: float,
):
    if (
        extflash_summary.status == TestStatus.SUCCESS
        and rf_summary.status == TestStatus.SUCCESS
    ):
        nic_test_status = TestStatus.SUCCESS
    else:
        nic_test_status = TestStatus.FAILED

    nic_summary = TestSummary("NIC", nic_test_status, time.time() - script_start)

    summary_print = "\n\nNIC Testing Summary (Serial Number: %s):\n" % nic_serial_number.decode("utf-8")

    summary_print += "_" * 80 + "\n\n"

    summary_print += build_individual_test_summary(extflash_summary)
    summary_print += build_individual_test_summary(rf_summary)

    summary_print += "\n" + "_" * 80 + "\n\n"

    summary_print += build_individual_test_summary(nic_summary)

    logging.info(summary_print)


if __name__ == "__main__":
    logging.basicConfig(
        format="%(levelname)s %(asctime)s %(message)s", level=logging.INFO
    )

    parser = argparse.ArgumentParser(fromfile_prefix_chars="@")

    # Serial parameters
    parser.add_argument(
        "--serial_port",
        type=str,
        help="Serial port used to communicate with the NIC.",
        required=True,
    )

    # Testing Router parameters
    parser.add_argument(
        "--test_router_address",
        type=validate_node_address,
        help="Test Router Address to communicate to during the RF testing.",
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

    # Required for the Test Summary written in External Flash
    parser.add_argument(
        "--serial_number",
        type=validate_serial_number,
        help="NIC Serial Number, that will be stored in external flash with the testing results.",
        required=True,
    )

    args = parser.parse_args()

    logging.debug(args)

    com = Communication()
    serial_initialization(args.serial_port, com)

    nic_serial_number = args.serial_number
    initialize_dut(args, com)

    extflash_summary = perform_external_flash_testing(com, extflash_summary)

    rf_summary = perform_rf_testing(com, rf_summary)

    print_testing_summary(extflash_summary, rf_summary, script_launch_time)

    exit(ExitCodes.SUCCESS)
