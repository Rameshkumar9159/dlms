# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import cbor2
import logging
from enum import IntEnum


class NodeProvisioningDataIds(IntEnum):
    WIREPAS_ENCRYPTION_KEY = 0
    WIREPAS_AUTHENTICATION_KEY = 1
    WIREPAS_NETWORK_ADDRESS = 2
    WIREPAS_NETWORK_CHANNEL = 3
    WIREPAS_NODE_ADDRESS = 4


class NicProvisioningDataIds(IntEnum):
    NIC_KEY_ENCRYPTION_KEY = 128
    NIC_AUTHENTICATION_KEY = 129
    NIC_ENCRYPTION_KEY = 130
    NIC_MR_PASSWORD = 131
    NIC_US_PASSWORD = 132
    NIC_FU_PASSWORD = 133
    NIC_FLAG_ID = 134
    NIC_BAUDRATE = 135
    NIC_INTERFACE_TYPE = 136


class ProvisioningDataReturnCode(IntEnum):
    SUCCESS = 0
    INVALID_STATE = 1
    INVALID_PARAMETER = 2
    INVALID_DATA = 3
    JOINING_LIB_ERROR = 4
    INTERNAL_ERROR = 5


class NicMode(IntEnum):
    HDLC = 0
    WRAPPER = 1
    UNDEFINED = 255


class WirepasProvisioning:
    """
    Class storing parameters
    """

    def __init__(
        self,
        encryption_key,
        authentication_key,
        network_address,
        network_channel,
        node_address,
        nic_key_encryption_key,
        nic_authentication_key,
        nic_encryption_key,
        nic_mr_password,
        nic_us_password,
        nic_fu_password,
        nic_flag_id,
        nic_baudrate,
        nic_interface_type,
    ):
        self.encryption_key = encryption_key
        self.authentication_key = authentication_key
        self.network_address = network_address
        self.network_channel = network_channel
        self.node_address = node_address
        self.nic_key_encryption_key = nic_key_encryption_key
        self.nic_authentication_key = nic_authentication_key
        self.nic_encryption_key = nic_encryption_key
        self.nic_mr_password = nic_mr_password
        self.nic_us_password = nic_us_password
        self.nic_fu_password = nic_fu_password
        self.nic_flag_id = nic_flag_id
        self.nic_baudrate = nic_baudrate

        try:
            self.nic_interface_type = NicMode[nic_interface_type].value
        except KeyError:
            self.nic_interface_type = None

    def _generate_provisioning_dict(self) -> dict:
        prov_dict = {
            NodeProvisioningDataIds.WIREPAS_ENCRYPTION_KEY.value: self.encryption_key,
            NodeProvisioningDataIds.WIREPAS_AUTHENTICATION_KEY.value: self.authentication_key,
            NodeProvisioningDataIds.WIREPAS_NETWORK_ADDRESS.value: self.network_address,
            NodeProvisioningDataIds.WIREPAS_NETWORK_CHANNEL.value: self.network_channel,
            NodeProvisioningDataIds.WIREPAS_NODE_ADDRESS.value: self.node_address,
            NicProvisioningDataIds.NIC_KEY_ENCRYPTION_KEY.value: self.nic_key_encryption_key,
            NicProvisioningDataIds.NIC_AUTHENTICATION_KEY.value: self.nic_authentication_key,
            NicProvisioningDataIds.NIC_ENCRYPTION_KEY.value: self.nic_encryption_key,
            NicProvisioningDataIds.NIC_MR_PASSWORD.value: self.nic_mr_password,
            NicProvisioningDataIds.NIC_US_PASSWORD.value: self.nic_us_password,
            NicProvisioningDataIds.NIC_FU_PASSWORD.value: self.nic_fu_password,
            NicProvisioningDataIds.NIC_FLAG_ID.value: self.nic_flag_id,
            NicProvisioningDataIds.NIC_BAUDRATE.value: self.nic_baudrate,
            NicProvisioningDataIds.NIC_INTERFACE_TYPE.value: self.nic_interface_type,
        }

        # Removing parameters with "None" value in the provisioning dictionary
        return {k: v for k, v in prov_dict.items() if v is not None}

    def get_provisioning_packet(self) -> bytes:
        prov_dict = self._generate_provisioning_dict()
        return cbor2.dumps(prov_dict)


def is_provisioning_dry_run_successful(return_code_frame: bytes) -> bool:
    return_code = cbor2.loads(return_code_frame)

    try:
        return_code = ProvisioningDataReturnCode(return_code)
    except:
        logging.warning("Unknown return code!")
        return False

    logging.info("Provisioning Packet Dry Run Return Code: " + return_code.name)
    if return_code != ProvisioningDataReturnCode.SUCCESS:
        return False

    return True


def decode_prov_dict(prov_read_data: bytes) -> bool:
    prov_dict = cbor2.loads(prov_read_data)
    if len(prov_dict) != (len(NodeProvisioningDataIds) + len(NicProvisioningDataIds)):
        logging.error("Missing parameters when decoding provisioning dictionary")
        return False

    nic_params = dict()
    for key in prov_dict:
        if key <= NodeProvisioningDataIds.WIREPAS_NODE_ADDRESS.value:
            new_key = NodeProvisioningDataIds(key).name
            nic_params[new_key] = prov_dict[key]
        elif key >= NicProvisioningDataIds.NIC_KEY_ENCRYPTION_KEY.value:
            new_key = NicProvisioningDataIds(int(key)).name
            nic_params[new_key] = prov_dict[key]

    nic_params[NodeProvisioningDataIds.WIREPAS_ENCRYPTION_KEY.name] = update_key_value(
        nic_params[NodeProvisioningDataIds.WIREPAS_ENCRYPTION_KEY.name]
    )
    nic_params[NodeProvisioningDataIds.WIREPAS_AUTHENTICATION_KEY.name] = (
        update_key_value(
            nic_params[NodeProvisioningDataIds.WIREPAS_AUTHENTICATION_KEY.name]
        )
    )

    nic_params[NodeProvisioningDataIds.WIREPAS_NETWORK_ADDRESS.name] = hex(
        nic_params[NodeProvisioningDataIds.WIREPAS_NETWORK_ADDRESS.name]
    )
    nic_params[NodeProvisioningDataIds.WIREPAS_NODE_ADDRESS.name] = hex(
        nic_params[NodeProvisioningDataIds.WIREPAS_NODE_ADDRESS.name]
    )

    nic_params[NicProvisioningDataIds.NIC_KEY_ENCRYPTION_KEY.name] = update_key_value(
        nic_params[NicProvisioningDataIds.NIC_KEY_ENCRYPTION_KEY.name]
    )

    nic_params[NicProvisioningDataIds.NIC_AUTHENTICATION_KEY.name] = update_key_value(
        nic_params[NicProvisioningDataIds.NIC_AUTHENTICATION_KEY.name]
    )
    nic_params[NicProvisioningDataIds.NIC_ENCRYPTION_KEY.name] = update_key_value(
        nic_params[NicProvisioningDataIds.NIC_ENCRYPTION_KEY.name]
    )
    nic_params[NicProvisioningDataIds.NIC_INTERFACE_TYPE.name] = NicMode(
        nic_params[NicProvisioningDataIds.NIC_INTERFACE_TYPE.name]
    ).name

    nic_params[NicProvisioningDataIds.NIC_MR_PASSWORD.name] = nic_params[
        NicProvisioningDataIds.NIC_MR_PASSWORD.name
    ].decode("utf-8")
    nic_params[NicProvisioningDataIds.NIC_US_PASSWORD.name] = nic_params[
        NicProvisioningDataIds.NIC_US_PASSWORD.name
    ].decode("utf-8")
    nic_params[NicProvisioningDataIds.NIC_FU_PASSWORD.name] = nic_params[
        NicProvisioningDataIds.NIC_FU_PASSWORD.name
    ].decode("utf-8")

    return nic_params


def print_provisioning_summary(nic_params: dict):
    summary = "\n\n\tProvisioned NIC Parameters Summary:\n"

    summary += "_" * 80 + "\n"
    summary += "\n".join("{!r}: {!r}".format(k, v) for k, v in nic_params.items())
    summary += "\n" + "_" * 80 + "\n"
    logging.info(summary)


def get_provisioning_status(nic_params: dict) -> bool:
    bad_parameters_counter = 0
    for key, value in nic_params.items():
        if value in ("Not set", 0, "0x0", "", "\x00\x00\x00", "UNDEFINED"):
            logging.error(key + " field (" + str(value) + ") is invalid!")
            bad_parameters_counter += 1

    return bad_parameters_counter < 1


def hide_security_keys(last_2_bytes: bytes):
    return (14 * "* ") + "{0:02x} {1:02x}".format(last_2_bytes[0], last_2_bytes[1])


def update_key_value(key: bytes) -> bytes:
    if key != b"":
        key = hide_security_keys(key)
    else:
        key = "Not set"

    return key
