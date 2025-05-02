# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#

import argparse
import logging
import nic_provisioning

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
    args = parser.parse_args()

    nic_provisioning.read_nic_parameters(args)
