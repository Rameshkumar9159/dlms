# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#

import argparse
import logging
import queue
import sys
import time
import serial

from colorama import Fore, Style
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

MAX_DUT_PER_JIG = 75
TAB_SIZE = 4
UNKNOWN_ID_CHAR = '❔'
NUL_CHAR = '\0'
METER_DEV_ID_LENGTH = 3
METER_SERIAL_NUM_LENGTH = 13

association_lvl_to_str = {
    "0": "None",
    "1": "PC",
    "2": "MR",
    "3": "PC+MR",
    "4": "US",
    "5": "PC+US",
    "6": "MR+US",
    "7": "PC+MR+US",
    "8": "FU",
    "9": "PC+FU",
    "10": "FU+MR",
    "11": "PC+MR+FU",
    "12": "US+FU",
    "13": "PC+US+FU",
    "14": "MR+US+FU",
    "15": "PC+MR+US+FU",
}


class ExitCodes(IntEnum):
    SUCCESS = 0
    SENDING_FAILED = 1
    RECEPTION_TIMEOUT = 2
    INVALID_PARAMETERS = 3
    TEST_FAILED = 4
    TEST_ALREADY_DONE = 5
    HDLC_PACKET_TOO_LARGE = 6
    SERIAL_ERROR = 7
    GENERIC_ERROR = 8


class MessageType(IntEnum):
    REQ_TEST_INIT = 0
    RSP_TEST_INIT = 1
    REQ_TEST_START = 2
    RSP_TEST_START = 3
    REQ_TEST_RESULT = 4
    RSP_TEST_RESULT = 5
    REQ_CONFIG_ROUTER = 240
    RSP_CONFIG_ROUTER = 241


class TestReqStatus(IntEnum):
    OK = 0
    CONFIG_ERROR = 1
    STATUS_GENERIC_ERROR = 2


class Communication:
    SERIAL_BAUDRATE = 115200
    ACK_NACK_TIMEOUT_S = 0.030
    MAX_TX_ATTEMPT = 3
    TIMEOUT_RSP_TEST_INIT_S = 3
    TIMEOUT_RSP_TEST_START_S = 3
    TIMEOUT_RSP_TEST_RESULT_S = 3
    EMBEDDED_HDLC_BUFFER_MAX_SIZE = 512

    ser = serial.Serial()

    hdlc_decode = Event()
    tx_seq = 0

    tx_queue = queue.Queue(MAX_TX_ATTEMPT)

    rx_ftype_queue = queue.Queue()
    rsp_test_init_queue = queue.Queue()
    rsp_test_start_queue = queue.Queue()
    rsp_test_result_queue = queue.Queue()


class TestStatus(IntEnum):
    SUCCESS = 0
    UNTESTED = 1
    FAILED = 2


class TestRecord():
    def __init__(self, dev_id="", app_ver="", mac_addr=0,
                    serial_com_status=TestStatus.UNTESTED, association_lvl_status=0,
                    rf_status=TestStatus.UNTESTED, rf_rx_res_rssi=0,
                    rf_rx_res_tx_pwr=0, rf_tx_res_rssi=0, rf_tx_res_tx_pwr=0
                    ):
        self.device_id = dev_id
        self.app_version = app_ver
        self.mac_address = mac_addr
        self.serial_com_status = serial_com_status
        self.association_lvl_status = association_lvl_status
        self.rf_status = rf_status
        self.rf_rx_res_rssi=rf_rx_res_rssi,
        self.rf_rx_res_tx_pwr=rf_rx_res_tx_pwr
        self.rf_tx_res_rssi=rf_tx_res_rssi
        self.rf_tx_res_tx_pwr=rf_tx_res_tx_pwr


def print_error_and_exit(message: str, error_code: ExitCodes):
    logging.error(Fore.RED + message + Fore.RESET)
    exit(error_code.value)


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
        print_error_and_exit(
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
        print_error_and_exit(msg, ExitCodes.SERIAL_ERROR)


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

    if type == MessageType.RSP_TEST_INIT:
        logging.debug("Received RSP_TEST_INIT")
        com.rsp_test_init_queue.put_nowait(data)
    elif type == MessageType.RSP_TEST_START:
        logging.debug("Received RSP_TEST_START")
        com.rsp_test_start_queue.put_nowait(data)
    elif type == MessageType.RSP_TEST_RESULT:
        logging.debug("Received RSP_TEST_RESULT")
        com.rsp_test_result_queue.put_nowait(data)
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
                logging.debug("Invalid frame: %s", ftype)
            read = bytes()
        except MessageError:
            # No HDLC frame detected.
            pass
        except FCSError:
            logging.warning("Bad FCS")
            pass


def initialize_test_router(args: argparse, com: Communication) -> int:

    logging.info("Initializing test router")

    rf_testing_parameters = pack("<bb", args.rx_rssi_threshold, args.tx_rssi_threshold)
    req_init_test_router_msg = rf_testing_parameters

    while send_data(com, req_init_test_router_msg, MessageType.REQ_TEST_INIT) is False:
        logging.warning(
            Fore.YELLOW + "Please connect the router to the Test Station" + Fore.RESET
        )

    try:
        init_test_data = com.rsp_test_init_queue.get(timeout=com.TIMEOUT_RSP_TEST_INIT_S)
    except queue.Empty:
        print_error_and_exit("RSP_TEST_INIT was not received in the given timeout!",
                                ExitCodes.RECEPTION_TIMEOUT
        )

    test_init_status, test_init_tr_mac_addr = unpack(
        "<BI", init_test_data
    )

    if test_init_status != TestReqStatus.OK:
        # This should never happen as test router always returns OK
        print_error_and_exit("Test router initialisation failed",
                                ExitCodes.INVALID_PARAMETERS
        )

    return test_init_tr_mac_addr

def start_test_procedure(com: Communication, test_duration: int):

    logging.info(f"Starting test procedure with duration '{test_duration}s'")

    if send_data(com, bytes(), MessageType.REQ_TEST_START) is False:
        print_error_and_exit("Could not send start test session request!",
                                ExitCodes.SENDING_FAILED
        )

    try:
        start_test_data = com.rsp_test_start_queue.get(timeout=com.TIMEOUT_RSP_TEST_START_S)
    except queue.Empty:
        print_error_and_exit("Start test session response not received!",
                                ExitCodes.RECEPTION_TIMEOUT
        )

    test_start_status, = unpack("<B", start_test_data)

    if test_start_status == TestReqStatus.STATUS_GENERIC_ERROR:
        print_error_and_exit("Could not start test session: generic error",
                                ExitCodes.GENERIC_ERROR
        )

    if test_start_status == TestReqStatus.CONFIG_ERROR:
        print_error_and_exit("Could not start test session: invalid test router configuration",
                                ExitCodes.INVALID_PARAMETERS
        )
    else:
        logging.info("Test procedure successfully started!")

    time.sleep(test_duration)

def get_test_results(com: Communication) -> list[TestRecord]:
    test_record_cnt = 0
    test_record_cnt_total = MAX_DUT_PER_JIG
    test_summary = []

    while (test_record_cnt < test_record_cnt_total):
        if send_data(com, test_record_cnt.to_bytes(1, byteorder='little'), MessageType.REQ_TEST_RESULT) is False:
            print_error_and_exit("Could not get test results",
                                    ExitCodes.SENDING_FAILED
            )

        try:
            test_record_data = com.rsp_test_result_queue.get(timeout=com.TIMEOUT_RSP_TEST_RESULT_S)
        except queue.Empty:
            print_error_and_exit(f"Get test record #{test_record_cnt} response not received!",
                                    ExitCodes.RECEPTION_TIMEOUT
            )

        test_record_cnt_total, = unpack("<B", test_record_data[0:1])

        if test_record_cnt_total > 0:
            # Test router has some DUT test result stored
            test_record = TestRecord()
            (test_record.device_id,
                test_record.app_version,
                test_record.mac_address,
                test_record.serial_com_status,
                test_record.association_lvl_status,
                test_record.rf_status,
                test_record.rf_rx_res_rssi,
                test_record.rf_rx_res_tx_pwr,
                test_record.rf_tx_res_rssi,
                test_record.rf_tx_res_tx_pwr
                ) = unpack("<16s10sIBBBbbbb", test_record_data[1:])
            # format strings
            test_record.device_id = test_record.device_id.decode('ascii')
            test_record.app_version = test_record.app_version.decode('ascii')
            test_summary.append(test_record)
        else:
            logging.warning(Fore.YELLOW + "No DUT test result received!" + Fore.RESET)

        test_record_cnt += 1

    return test_summary

# Generate tabulation using spaces
def _gen_tabulation(tab_count: int):
    return ' ' * tab_count * TAB_SIZE

def _build_testing_summary_header(test_date_time: str, test_duration: int, test_record_count: int, test_router_address: int) -> str:
    header = "\n>DUT(s) testing summary<\n"
    header += _gen_tabulation(1) + f"Date:               {test_date_time}\n"
    header += _gen_tabulation(1) + f"Duration (s):       {test_duration}\n"
    header += _gen_tabulation(1) + f"Record count:       {test_record_count}\n"
    header += _gen_tabulation(1) + f"Router MAC address: {test_router_address}\n"
    header += ">\n"

    return header

def _build_colored_test_status_string(test_status: TestStatus) -> str:
    test_status_str = Style.BRIGHT
    if test_status == TestStatus.SUCCESS:
        test_status_str += (
            Fore.GREEN
            + "PASS"
            + Style.RESET_ALL
        )
    elif test_status == TestStatus.FAILED:
        test_status_str += (
            Fore.RED
            + "FAILED"
            + Style.RESET_ALL
        )
    else:
        test_status_str += (
            Fore.YELLOW
            + "UNTESTED"
            + Style.RESET_ALL
        )

    return test_status_str

def _build_association_lvl_status_string(asso_lvl: int) -> str:
    asso_status_str = ""
    try:
        asso_status_str = association_lvl_to_str[str(asso_lvl)]
    except KeyError:
        logging.error(f"Unknown association level value received: {asso_lvl}")
        asso_status_str = f"Unknown ({asso_lvl})"

    return asso_status_str


def _build_testing_summary_body(test_records: list[TestRecord]) -> str:
    body = ''

    record_cnt = 1
    for item in test_records:
        body += _gen_tabulation(1) + f"- #{record_cnt} -\n"

        # Format device ID and meter serail number
        dev_id = item.device_id[:METER_DEV_ID_LENGTH]
        meter_serial = item.device_id[METER_DEV_ID_LENGTH:]

        if dev_id == NUL_CHAR * METER_DEV_ID_LENGTH:
            # Set string to unknow state
            logging.debug("Device ID not read")
            dev_id = METER_DEV_ID_LENGTH * UNKNOWN_ID_CHAR
        if meter_serial == NUL_CHAR * METER_SERIAL_NUM_LENGTH:
            logging.debug("Meter serial number not read")
            meter_serial = METER_SERIAL_NUM_LENGTH * UNKNOWN_ID_CHAR

        body += _gen_tabulation(1) + f"{dev_id}[{meter_serial}]\n"
        body += _gen_tabulation(2) + f"App version: {item.app_version}\n"
        body += _gen_tabulation(2) + f"Wirepas MAC address: {item.mac_address}\n"
        body += _gen_tabulation(2) + "Test UART:            " + _build_colored_test_status_string(item.serial_com_status) + "\n"
        body += _gen_tabulation(2) + "Test RF:              " + _build_colored_test_status_string(item.rf_status) + "\n"
        body += _gen_tabulation(7) + f"  ├ RX RSSI: {item.rf_rx_res_rssi} dBm (@ 0 dBm)\n"
        body += _gen_tabulation(7) + f"  └ TX RSSI: {item.rf_tx_res_rssi} dBm (@ 0 dBm)\n"
        body += _gen_tabulation(2) + "Association lvl:      " + _build_association_lvl_status_string(item.association_lvl_status) + "\n"
        record_cnt += 1

    return body

def _build_testing_summary_footer() -> str:
    return "><\n"

def print_testing_summary(test_date_time: str, test_duration: int, test_router_mac_addr: int, test_records: list[TestRecord]):
    summary_print = _build_testing_summary_header(test_date_time, test_duration, len(test_records), test_router_mac_addr)
    summary_print += _build_testing_summary_body(test_records)
    summary_print += _build_testing_summary_footer()

    logging.info(summary_print)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(fromfile_prefix_chars="@")

    # Serial parameters
    parser.add_argument(
        "--serial_port",
        type=str,
        help="Serial port used to communicate with the Test router.",
        required=True,
    )

    # RF testing parameters
    parser.add_argument(
        "--rx-rssi-threshold",
        type=int,
        default=0,
        help="""RSSI threshold (dBm) for RF testing of DUT RX path. Value given should be normalised to 0 dBm of
                transmission power.
                If provided Tx threshold must be set."""
    )

    parser.add_argument(
        "--tx-rssi-threshold",
        type=int,
        default=0,
        help="""RSSI threshold (dBm) for RF testing of DUT TX path. Value given should be normalised to 0dBm of
                transmission power.
                If provided Rx threshold must be set."""
    )

    # Test procedure parameter
    parser.add_argument(
    "--test-duration",
    type=int,
    default=90,
    help="""Duration of the test.
            The DUTs test results will be queried from the test router after this amount of time"""
    )

    # Log parameters
    parser.add_argument("--log-level", default="info", type=str,
                        choices=["debug", "info", "warning", "error", "critical"],
                        help="Default to 'info'. Log level to be displayed. "
                        "It has to be chosen between 'debug', 'info', 'warning', 'error' and 'critical'")

    args = parser.parse_args()

    logging.basicConfig(format='%(asctime)s | [%(levelname)s] %(filename)s:%(lineno)d:%(message)s', level=args.log_level.upper(),
                        handlers=[
                            logging.StreamHandler(sys.stdout),
                            logging.FileHandler("eol_testing_app.log", mode="a", encoding="utf-8")
                        ]
    )

    logging.debug(args)

    # Check if partially configured RF threshold is provided
    if (args.rx_rssi_threshold == 0) != (args.tx_rssi_threshold == 0) :
        print_error_and_exit("RF test configuration invalid. Both Rx and Tx threshold must be provided",
                                ExitCodes.INVALID_PARAMETERS)

    com = Communication()
    serial_initialization(args.serial_port, com)

    test_router_mac_addr = initialize_test_router(args, com)
    logging.debug(f"Test router address: {test_router_mac_addr}")

    test_duration = args.test_duration

    while True:
        choice = input("s to [s]start a test session and e to [e]xit\n")

        if choice == 's':
            # test start time in UTC as string (YYYY-MM-DD hh:mm:ss)
            test_date_time = time.strftime('%Y-%m-%d %H:%M:%S',time.gmtime())
            start_test_procedure(com, test_duration)
            test_summary = get_test_results(com)

            if (not test_summary) == False:
                # Some test result to print
                print_testing_summary(test_date_time, test_duration, test_router_mac_addr, test_summary)
            continue
        if choice == 'e':
            break

    exit(ExitCodes.SUCCESS)
