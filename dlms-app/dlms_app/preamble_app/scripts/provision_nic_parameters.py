# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#

import argparse
import logging
import nic_provisioning


SECRET_MAX_SIZE = 32
KEY_SIZE = 16
FLAG_ID_LENGTH = 3
MINIMUM_NETWORK_ADDRESS = 1
MAXIMUM_NETWORK_ADDRESS = 0xFFFFFF
MINIMUM_NETWORK_CHANNEL = 1
MAXIMUM_NETWORK_CHANNEL = 12
MINIMUM_NODE_ADDRESS = 1
# 0x0xFFFFFFFE => APP_ADDR_ANYSINK
# 0x0xFFFFFFFF => APP_ADDR_BROADCAST
MAXIMUM_NODE_ADDRESS = 0xFFFFFFFD


def validate_key(key: str) -> bytes:
    byte_data = bytes.fromhex(key)
    if len(byte_data) != KEY_SIZE:
        raise argparse.ArgumentTypeError(
            "Given key must be " + str(KEY_SIZE) + " bytes long"
        )
    return byte_data


def validate_flag_id(flag_id: str) -> str:
    if len(flag_id) != FLAG_ID_LENGTH or flag_id.isalpha() is False:
        raise argparse.ArgumentTypeError(
            "Given Flag ID Must be composed of "
            + str(FLAG_ID_LENGTH)
            + " alphabetic characters"
        )

    # From https://www.dlms.com/flag-id/
    # Flag ID can be any 3 alphabetic character string (upper or lower case insensitive)
    return flag_id


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


def validate_password(password: str) -> str:
    if len(password) > SECRET_MAX_SIZE:
        raise argparse.ArgumentTypeError(
            "Password must be " + str(SECRET_MAX_SIZE) + " characters long maximum"
        )
    return password


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

    # Wirepas parameters
    parser.add_argument(
        "--encryption_key",
        type=validate_key,
        help="Encryption Key used by the NIC. Must be specified as a 16 bytes value.",
        default=None,
    )
    parser.add_argument(
        "--authentication_key",
        type=validate_key,
        help="Authentication Key used by the NIC. Must be specified as a 16 bytes value.",
        default=None,
    )
    parser.add_argument(
        "--network_address",
        type=validate_network_address,
        help="Network address used by the NIC.",
        default=None,
    )
    parser.add_argument(
        "--network_channel",
        type=validate_network_channel,
        help="Network channel used by the NIC.",
        default=None,
    )
    parser.add_argument(
        "--node_address",
        type=validate_node_address,
        help="Node Address used by the NIC.",
        required=True,
    )

    # NIC Parameters
    parser.add_argument(
        "--nic_key_encryption_key",
        type=validate_key,
        help="Key Encryption key used by the NIC to communicate with the meter. Must be specified as a 16 bytes value.",
        default=None,
    )
    parser.add_argument(
        "--nic_authentication_key",
        type=validate_key,
        help="Authentication key used by the NIC to communicate with the meter. Must be specified as a 16 bytes value.",
        default=None,
    )
    parser.add_argument(
        "--nic_encryption_key",
        type=validate_key,
        help="Global Unicast Encryption key used by the NIC to communicate with the meter. Must be specified as a 16 bytes value.",
        default=None,
    )
    parser.add_argument(
        "--nic_mr_password",
        type=validate_password,
        help="NIC MR Password must be "
        + str(SECRET_MAX_SIZE)
        + " characters long maximum.",
        default=None,
    )
    parser.add_argument(
        "--nic_us_password",
        type=validate_password,
        help="NIC US Password must be "
        + str(SECRET_MAX_SIZE)
        + " characters long maximum.",
        default=None,
    )
    parser.add_argument(
        "--nic_fu_password",
        type=validate_password,
        help="NIC FU Password must be "
        + str(SECRET_MAX_SIZE)
        + " characters long maximum.",
        default=None,
    )
    parser.add_argument(
        "--nic_flag_id",
        type=validate_flag_id,
        help="NIC Flag ID. Must be specified as "
        + str(FLAG_ID_LENGTH)
        + " ISO Alphabetic characters.",
        default=None,
    )
    parser.add_argument(
        "--nic_baudrate",
        type=int,
        choices=[
            110,
            150,
            300,
            1200,
            2400,
            4800,
            9600,
            19200,
            38400,
            57600,
            115200,
            230400,
            460800,
            921600,
        ],
        help="NIC Baudrate used to communicate with the meter.",
        default=None,
    )
    parser.add_argument(
        "--nic_interface_type",
        type=lambda s: s.upper(),
        choices=["HDLC", "WRAPPER"],
        help="NIC Interface Type used for the communication with the meter. Either hdlc or wrapper, case insensitive.",
        default=None,
    )
    args = parser.parse_args()

    nic_provisioning.provision_nic_parameters(args)
