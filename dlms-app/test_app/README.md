# README.md

## Introduction

This Wirepas Application and Python Script handle the NIC Testing (External Flash and RF).

The focus here is on the script usage.

## Requirements installation

This script needs some external modules.

First move to [/test_app](/test_app)

To install them please use:

```Python
pip install -r requirements.txt
```

## General information

### Communication

The communication is performed through the meter port at 115200 bauds.

### Setup

It is required to have the following to perform the NIC testing:

- A PC, Running the Python Script
- A Serial USB Converter, that will connect the PC to the NIC's Meter Port
- A NIC

Then, the process to test the NIC is the following

1. Connect the PC to the NIC using the Serial USB Converter
2. Launch the script with the right parameters
3. Wait until the testing summary is printed

## `test.py`

### Script help

To access the scripts' help please use the following command:

```Python
python3 test.py -h
```

### Required Parameters

|Parameter|Description|Example|
|-|-|-|
| `--serial_port` | Serial port used to communicate with the NIC.|`--serial_port /dev/ttyUSB0`|
| `--test_router_address` | Test Router Address.|`--test_router_address 0x12347678` or `--test_router_address 305419896`|
| `--network_address` | Network address used by the NIC during the RF testing.|`--network_address 0x123476` or `--network_address 1193046`|
| `--network_channel` | Network channel used by the NIC during the RF testing.|`--network_channel 1`|
| `--encryption_key` | Encryption Key used by the NIC during the RF testing. Must be specified as a 16 bytes value.|`--encryption_key AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA`|
| `--authentication_key` | Authentication Key used by the NIC during the RF testing. Must be specified as a 16 bytes value.|`--authentication_key BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB`|
| `--serial_number` | NIC Serial Number, that will be stored in external flash with the testing results.|`NIC_SERIAL_NUMBER`|

### Launching the script (with the required parameters)

```Python
python3 test.py --serial_port /dev/ttyUSB0  --encryption_key 33333333333333333333333333333333 --authentication_key 44444444444444444444444444444444 --network_address 0xF0EE0F --network_channel 1 --test_router_address 1 --serial_number NIC_SERIAL_NUMBER
```

#### Advanced script launching

It's possible to create a file that stores these parameter to avoid having to write them every time.

**However, we advise not to specify the serial_number parameter in it, as it shall be unique.**

**We also advise not to specify the serial port inside it, if there are multiple Serial USB Converters in use to communicate with different NICs.**

Here's what the file could look like:

Name of the file: `parameters`

```txt
--encryption_key
33333333333333333333333333333333
--authentication_key
44444444444444444444444444444444
--network_address
0xF0EE0F
--network_channel
1
--test_router_address
1
```

Then, the script can be executed like this:

```Python
python3 test.py --serial_port /dev/ttyUSB0 --serial_number NIC_SERIAL_NUMBER @parameters
```
