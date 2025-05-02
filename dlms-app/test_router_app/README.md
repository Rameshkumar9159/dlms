# README.md

## Introduction

This Wirepas Application (**app.c**) and the Python Script (**configure_router.py**) handle the Configuration of the Testing Router when in the 'NIC Testing' manufacturing phase.
When in the End of Line (EOL) testing phase, the application controls (with proper compilation option enabled) the testing process coupled with the **eol_testing_app.py** Python script.

The usage in the different testing phase will be explained in the next sections.

## Requirements installation

The `configure_router.py` and `eol_testing_app.py` scripts need some common external modules.

First move to the [/test_router_app](/test_router_app) folder.

To install them please use:

```Python
pip install -r requirements.txt
```

## General information

### Communication

The communication is performed with the Testing Router through the meter port at 115200 bauds.

### Setup

It is required to have the following to configure the Testing Router:

- A PC, Running the Python Script
- A Serial USB Converter, that will connect the PC to the Testing Router's meter Port
- A Board that will act as the testing router which is flashed with the Wirepas application configured in the NIC or EOL testing mode

The Testing router then needs to have its Network Parameters configured as follows:

1. Connect the PC to the Testing Router using the Serial USB Converter
2. Launch the [**configure_router.py**](#configure_router-script) script with the right parameters
3. Wait until the configuration result is printed

#### End of Line Testing
In this manufacturing phase, the Testing Router needs to be flashed with the Wirepas Application compiled in "EOL mode".
This can be enabled by passing the `eol_mode=true` option to the compilation command. If ommited or set to a value different from *true*,
the Testing Router will operate in NIC testing mode. From this point the network parameters can be configured as described above and in  
[configure_router script](#configure_router-script) section. Finally, the test session can be controlled via the `eol_testing_app.py` Python script
as explained in [eol_testing_app script](#eol_testing_app-script).


## configure_router script

### Script help

To access the script's help please use the following command:

```Python
python3 configure_router.py -h
```

### Required Parameters

|Parameter|Description|Example|
|-|-|-|
| `--serial_port` | Serial port used to communicate with the NIC.|`--serial_port /dev/ttyUSB0`|
| `--test_router_address` | Test Router Address to communicate to during the RF testing.|`--test_router_address 0x12347678` or `--test_router_address 305419896`|
| `--network_address` | Network address used by the NIC during the testing (RF or EOL).|`--network_address 0x123476` or `--network_address 1193046`|
| `--network_channel` | Network channel used by the NIC during the testing (RF or EOL).|`--network_channel 1`|
| `--encryption_key` | Encryption Key used by the NIC during the testing (RF or EOL). Must be specified as a 16 bytes value.|`--encryption_key AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA`|
| `--authentication_key` | Authentication Key used by the NIC during the testing (RF or EOL). Must be specified as a 16 bytes value.|`--authentication_key BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB`|

### Launching the script (with the required parameters)

```Python
python3 configure_router.py --serial_port /dev/ttyUSB0  --test_router_address 1 --encryption_key 33333333333333333333333333333333 --authentication_key 44444444444444444444444444444444 --network_address 0xF0EE0F --network_channel 1
```

#### Advanced script launching

It's possible to create a file that stores these parameters to avoid having to write them every time.

**We advise not to specify the serial port inside it, if there are multiple Serial USB Converters in use to communicate with other devices.**

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
python3 configure_router.py --serial_port /dev/ttyUSB0 @parameters
```
## eol_testing_app script

This script is in charge of controlling the End Of Line testing of meters with their respective NICs via a Testing Router device.
It expects that the application running on the NICs implement this EOL testing matching feature. This is the case for this DLMS application.

### Script help

To access the script's help please use the following command:

```Python
python3 eol_testing_app.py -h
```

### Required Parameters

|Parameter|Description|Example|
|-|-|-|
| `--serial_port` | Serial port used to communicate with the Testing Router.|`--serial_port /dev/ttyUSB0`|
| `--test-duration` | End Of Line testing duration in seconds. Test results will be retrieved and displayed after this time. Default = 90 |`--test-duration 120`|
| `--tx-rssi-threshold` | Acceptance threshold in dBm (normalized RSSI value to 0dBm) for meter RF in transmission. Must be set at the same time as `--rx-rssi-threshold`. If not provided, RF test will be disabled |`--tx-rssi-threshold -70`|
| `--rx-rssi-threshold` | Acceptance threshold in dBm (normalized RSSI value to 0dBm) for meter RF in reception. Must be set at the same time as `--tx-rssi-threshold`. If not provided, RF test will be disabled |`--rx-rssi-threshold -75`|

### Launching the script (with the required parameters)

```Python
python3 eol_testing_app.py --serial_port /dev/ttyUSB0
```

#### Advanced script launching

It's possible to create a file that stores these parameters to avoid having to write them every time.

**We advise not to specify the serial port inside it, if there are multiple Serial USB Converters in use to communicate with other devices.**

Here's what the file could look like:

Name of the file: `parameters`

```txt
--tx-rssi-threshold
-70
--rx-rssi-threshold
-75
--test-duration
120
```

Then, the script can be executed like this:

```Python
python3 eol_testing_app.py --serial_port /dev/ttyUSB0 @parameters
```
