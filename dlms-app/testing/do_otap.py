#!/usr/bin/env python3

# Copyright 2021 Wirepas Ltd. All Rights Reserved.
#
# See file LICENSE.txt for full license details.
#
import argparse
from wirepas_mqtt_library import WirepasNetworkInterface, WirepasOtapHelper
import wirepas_mesh_messaging as wmm
import logging
from random import choice
from time import sleep, time

OTAP_TIMEOUT_MINUTES = 5

def node_list(val):
    try:
        return [int(x) for x in val.split(',')]
    except:
        logging.error("Cannot parse node list")
        exit(1)

if __name__ == "__main__":

    logging.basicConfig(format='%(levelname)s %(asctime)s %(message)s', level=logging.INFO)

    parser = argparse.ArgumentParser(fromfile_prefix_chars='@')
    parser.add_argument('--host',
                        help="MQTT broker address")
    parser.add_argument('--port', default=8883,
                        type=int,
                        help="MQTT broker port")
    parser.add_argument('--username', default='mqttmasteruser',
                        help="MQTT broker username")
    parser.add_argument('--password',
                        help="MQTT broker password")
    parser.add_argument('--insecure',
                        dest='insecure',
                        action='store_true',
                        help="MQTT use unsecured connection")

    parser.add_argument('--network',
                        type=int,
                        help="Network address concerned by scratchpad")

    parser.add_argument('--file',
                        help="Scratcphad to use")
    parser.add_argument('--version_file',
                        help="Version file generated at build time (version.txt)")

    parser.add_argument('--node_list',
                        type=node_list,
                        help="List of nodes in the network")

    args = parser.parse_args()

    wni = WirepasNetworkInterface(args.host,
                                  args.port,
                                  args.username,
                                  args.password,
                                  strict_mode=False)

    otapHelper = WirepasOtapHelper(wni,
                                   args.network)

    # Check the application version from version.txt
    target_app_version = None
    with open(args.version_file) as f:
        for line in f.readlines():
            try:
                version = line.split("app_version=")[1]
                target_app_version = tuple(int(x) for x in version.split("."))
            except IndexError:
                pass

    if target_app_version is None:
        logging.error("Cannot determine version of app")
        exit(1)

    logging.info("Target app version is: " + str(target_app_version))

    # Load scratchpad
    current_target_seq_set = otapHelper.get_target_scratchpad_seq_list()
    logging.info("Sequences already in used: " + str(current_target_seq_set))

    # Take a sequence from 1-254 that is not in the current set
    seq = choice([i for i in range(1,254) if i not in current_target_seq_set])
    logging.info("Sequence chosen: " + str(seq))
    if not otapHelper.load_scratchpad_to_all_sinks(args.file, seq):
        logging.error("Cannot load scratchpad to all sinks")
        exit(1)

    logging.info("Set propagate and process")
    if not otapHelper.set_propagate_and_process_scratchpad_to_all_sinks():
        logging.error("Cannot set propagate and process")
        exit(1)

    # Wait for max 5 minutes for scratchpad to be processed
    now = time()
    node_list = args.node_list
    node_list.sort()
    return_code = 0

    try:
        logging.info("Wait for scratchpad propagation")
        while True:
            # Send downlink remote status every 10s
            # We have a pretty small network so agressive downlink is fine
            node_status = otapHelper.send_remote_scratchpad_status()
            if not node_status:
                logging.info("Cannot send status from sinks, probably doing scratchpad exchange")
                # Wait a bit longer
                sleep(30)

            # Wait 5s
            sleep(5)

            # Get list of node status
            nodes = otapHelper.get_current_nodes_status().copy()

            # Check received status against our node list
            nodes_updated = list()
            for node_id, node in nodes.items():
                logging.info(" {:10d} | {} ".format(
                    node_id,
                    node['app_version']
                ))
                if node['app_version'] == target_app_version:
                    nodes_updated.append(node_id)
                    logging.info("Target for " + str(node_id) + " reached")

            # Get missing nodes
            missing_nodes = set(node_list).difference(nodes_updated)

            if len(missing_nodes) == 0:
                logging.info("All node updated!")
                break
            else:
                logging.info("Still waiting to update nodes: " + str(missing_nodes))

            if (time() - now) > (OTAP_TIMEOUT_MINUTES * 60):
                raise TimeoutError("Not all the node updated within " + str(OTAP_TIMEOUT_MINUTES) + " minutes")
    except TimeoutError:
        logging.error("Timeout error in otap")
        return_code = 1
    finally:
        # Set to no otap
        logging.info("Set target to no otap")
        if not otapHelper.set_no_otap_to_all_sinks():
            logging.error("Cannot set no otap on all sinks")
            return_code = 1

    logging.info("All done!")
    exit(return_code)

