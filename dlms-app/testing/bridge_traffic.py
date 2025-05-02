#!/usr/bin/env python3

from wirepas_mqtt_library import WirepasNetworkInterface
import wirepas_mesh_messaging as wmm
import logging
import argparse
from time import sleep, time
from datetime import datetime
import socket
import threading

UDP_IP = "127.0.0.1"

connection_to_broker = threading.Event()
connected_to_broker = False
sock = None
remote_address = None
node = None

def on_data_received_passthru(data):

    if data.source_address != node:
        logging.info("Discarding traffic from node %d", data.source_address)
        return

    logging.info("-------------------------")
    logging.info("Forwading pass-through message from %d to %s" % (data.source_address, remote_address))
    logging.info("%s" % data.data_payload.hex())

    if sock:
        sock.sendto(data.data_payload, remote_address)


def node_list(val):
    try:
        return [int(x) for x in val.split(',')]
    except:
        logging.error("Cannot parse node list")
        exit(1)


def on_mqtt_connection_change_cb(connected, error_code):
    global connected_to_broker
    connected_to_broker = connected
    connection_to_broker.set()


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
    parser.add_argument('--node',
                        type=int,
                        help="Target node (mandatory)")
    parser.add_argument('--gateway',
                        help="Target gateway (optional)")
    parser.add_argument('--udpport', default=5005,
                        type=int,
                        help="UDP port")

    args = parser.parse_args()
    if args.node is None:
        logging.error("The target node is mandatory")
        exit()

    node = args.node
    wni = WirepasNetworkInterface(args.host,
                                  args.port,
                                  args.username,
                                  args.password,
                                  strict_mode=False,
                                  connection_cb=on_mqtt_connection_change_cb)


    # wait connection to broker
    connection_to_broker.wait()
    if not connected_to_broker:
        exit()

    # UDP bridge
    logging.info("Bridging messages to node %u" % args.node)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, args.udpport))
    sock.settimeout(1) # to be able to exit the program with CTRL-C

    wni.register_data_cb(on_data_received_passthru, network=args.network, src_ep=65, dst_ep=65)

    logging.info("listening on UDP port: %u" % args.udpport)
    while connected_to_broker:
        try:
            data, remote_address = sock.recvfrom(1500)
            logging.info("forwarding message: %s" % data.hex())

            for gw, sink, config in wni.get_sinks(network_address=args.network, gateway=args.gateway):
                try:
                    res = wni.send_message(gw, sink, args.node, 65, 65, data)
                    if res != wmm.GatewayResultCode.GW_RES_OK:
                        logging.error("Cannot send data to %s:%s res=%s" % (gw, sink, res))
                except TimeoutError:
                    logging.error("Cannot send data to %s:%s", gw, sink)

        except socket.timeout:
            pass


    logging.info("Exiting!")