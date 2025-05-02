# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#

import argparse
from colorama import Fore
import queue
import logging
import wirepas_provisioning
import serial
import sys

from wirepas_provisioning import WirepasProvisioning
from threading import Thread, Event
from struct import pack, unpack
from enum import Enum
from time import sleep


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


class ExitCodes(Enum):
    SUCCESS = 0
    SENDING_FAILED = 1
    RECEPTION_TIMEOUT = 2
    INVALID_PARAMETERS = 3
    MISSING_PARAMETERS = 4
    HDLC_PACKET_TOO_LARGE = 5
    CALL_TO_MAIN = 6
    SERIAL_ERROR = 7


class MessageType(Enum):
    SEND_PROVISIONING_PACKET = 0
    RECEIVE_PROVISIONING_RETURN_CODE = 1
    SEND_DEVICE_REBOOT_REQUEST = 2
    SEND_READ_PARAMETERS_REQUEST = 3
    RECEIVE_READ_PARAMETERS_RESPONSE = 4


PROVISIONING_CHALLENGE = b"WPP\n"
PROVISIONING_CHALLENGE_RESP = b"OK\n"
PROVISIONING_CHALLENGE_PERIOD_S = 0.05  # Every 50 ms


class Communication:
    SERIAL_BAUDRATE = 115200
    ACK_NACK_TIMEOUT_S = 0.500
    MAX_TX_ATTEMPT = 3
    DRY_RUN_RECEIVE_TIMEOUT_S = 2
    READ_PARAMETERS_TIMEOUT_S = 2
    EMBEDDED_HDLC_BUFFER_MAX_SIZE = 512

    ser = serial.Serial()

    hdlc_decode = Event()
    tx_seq = 0

    tx_queue = queue.Queue(MAX_TX_ATTEMPT)
    rx_challenge_queue = queue.Queue()
    rx_ftype_queue = queue.Queue()
    provisioning_success_queue = queue.Queue()
    read_parameters_queue = queue.Queue()


def error_print_and_exit(message: str, error_code: ExitCodes):
    logging.error(Fore.RED + message)
    exit(error_code.value)


def success_print(message):
    logging.info(Fore.GREEN + message)


def send_data(com: Communication, data: bytes, type: MessageType) -> bool:
    # Generating header
    header = pack("<BB", 0, type.value)
    message = header + data
    hdlc_frame = frame_data(message, FRAME_DATA, com.tx_seq)

    if len(hdlc_frame) > com.EMBEDDED_HDLC_BUFFER_MAX_SIZE:
        error_print_and_exit(
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
                "Sending",
                len(packet),
                "bytes long data frame:",
                packet.hex(),
                "with seq:",
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

    except serial.SerialException:
        error_print_and_exit("Serial connection problem!", ExitCodes.SERIAL_ERROR)


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

    if type == MessageType.RECEIVE_PROVISIONING_RETURN_CODE:
        if wirepas_provisioning.is_provisioning_dry_run_successful(data) is True:
            logging.debug("Dry Run Successful! Sending reboot request")
            com.provisioning_success_queue.put(True)
        else:
            logging.debug("Dry Run returned an error! No reboot request sent!")
            com.provisioning_success_queue.put(False)
    elif type == MessageType.RECEIVE_READ_PARAMETERS_RESPONSE:
        logging.debug("Read Request Response Received")
        com.read_parameters_queue.put(data)

    else:
        logging.warning("Unknown type received", type)


def rx_callback(com: Communication):
    read = bytes()
    while True:
        read += com.ser.read(1)
        if com.hdlc_decode.is_set():
            # HDLC Reading
            try:
                get_data_reset()
                data, ftype, seq_no = get_data(read)
                logging.debug(ftype, data.hex(), seq_no)
                com.rx_ftype_queue.put(ftype)
                if ftype == FRAME_ACK:
                    logging.debug("Ack for seq:", seq_no)
                elif ftype == FRAME_NACK:
                    logging.debug("Nack for seq:", seq_no)
                elif ftype == FRAME_DATA:
                    logging.debug("Message received: ", data.hex())
                    # Send a ack
                    com.ser.write(frame_data("", FRAME_ACK, seq_no))
                    parse_message_received(com, data)
                else:
                    logging.debug(read.hex())
                    logging.debug("Invalid frame", ftype)
                read = bytes()
            except MessageError:
                # No HDLC frame detected.
                pass
            except FCSError:
                logging.warning("Bad FCS")
                pass
        else:
            # Serial Reading (No protocol)
            if PROVISIONING_CHALLENGE_RESP in read:
                com.rx_challenge_queue.put(True)


def wait_for_challenge_response(com: Communication):
    # Disable HDLC Serial Reading
    com.hdlc_decode.clear()

    logging.info("Sending Provisioning Challenge...")
    while com.rx_challenge_queue.empty():
        com.ser.write(PROVISIONING_CHALLENGE)
        sleep(PROVISIONING_CHALLENGE_PERIOD_S)

    com.rx_challenge_queue.queue.clear()

    # Enable HDLC Serial Reading
    com.hdlc_decode.set()
    logging.info("Received Provisioning Challenge Response!")


def perform_parameters_provisioning(com: Communication, wp_prov: WirepasProvisioning):
    provisioning_packet = wp_prov.get_provisioning_packet()

    logging.info("Sending Provisioning Packet")
    if (
        send_data(com, provisioning_packet, MessageType.SEND_PROVISIONING_PACKET)
        is False
    ):
        error_print_and_exit(
            "Sending Provisioning Packet isn't successful!", ExitCodes.SENDING_FAILED
        )

    try:
        validation_run_success = com.provisioning_success_queue.get(
            timeout=com.DRY_RUN_RECEIVE_TIMEOUT_S
        )
    except queue.Empty:
        error_print_and_exit(
            "Provisioning validation run return code not received in the given timeout!",
            ExitCodes.RECEPTION_TIMEOUT,
        )

    if validation_run_success is False:
        error_print_and_exit(
            "Provisioning validation run was not successful!",
            ExitCodes.INVALID_PARAMETERS,
        )

    logging.info("Applying parameters to the NIC")

    if send_data(com, bytes(), MessageType.SEND_DEVICE_REBOOT_REQUEST) is False:
        error_print_and_exit(
            "Sending Device Reboot Request not successful!", ExitCodes.SENDING_FAILED
        )


def perform_parameters_reading(com: Communication):
    logging.info("Sending request to read NIC Parameters")
    if send_data(com, bytes(), MessageType.SEND_READ_PARAMETERS_REQUEST) is False:
        error_print_and_exit(
            "Sending Read Parameters Request not successful!", ExitCodes.SENDING_FAILED
        )

    try:
        nic_parameters_data = com.read_parameters_queue.get(
            timeout=com.READ_PARAMETERS_TIMEOUT_S
        )
    except queue.Empty:
        error_print_and_exit(
            "Read Parameters response was not received in the given timeout!",
            ExitCodes.RECEPTION_TIMEOUT,
        )

    nic_parameters_data = wirepas_provisioning.decode_prov_dict(nic_parameters_data)

    wirepas_provisioning.print_provisioning_summary(nic_parameters_data)

    prov_sucess = wirepas_provisioning.get_provisioning_status(nic_parameters_data)

    if prov_sucess is True:
        success_print("NIC is successfully provisioned!")
    else:
        error_print_and_exit(
            "Missing parameters in the device!", ExitCodes.MISSING_PARAMETERS
        )


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


def provision_nic_parameters(args: argparse):
    logging.info("Starting NIC Provisioning")

    wp_prov = WirepasProvisioning(
        args.encryption_key,
        args.authentication_key,
        args.network_address,
        args.network_channel,
        args.node_address,
        args.nic_key_encryption_key,
        args.nic_authentication_key,
        args.nic_encryption_key,
        args.nic_mr_password,
        args.nic_us_password,
        args.nic_fu_password,
        args.nic_flag_id,
        args.nic_baudrate,
        args.nic_interface_type,
    )

    com = Communication()
    serial_initialization(args.serial_port, com)

    wait_for_challenge_response(com)
    perform_parameters_provisioning(com, wp_prov)

    # Waiting for NIC to reboot before asking the parameters
    logging.info("Waiting for NIC to reboot...")

    wait_for_challenge_response(com)
    perform_parameters_reading(com)

    sys.exit(ExitCodes.SUCCESS.value)


def read_nic_parameters(args: argparse):
    com = Communication()
    serial_initialization(args.serial_port, com)

    wait_for_challenge_response(com)
    perform_parameters_reading(com)

    sys.exit(ExitCodes.SUCCESS.value)


if __name__ == "__main__":
    error_print_and_exit(
        "Unavailable. Please use 'provision_nic_parameters.py' or 'read_nic_parameters.py'",
        ExitCodes.CALL_TO_MAIN,
    )
