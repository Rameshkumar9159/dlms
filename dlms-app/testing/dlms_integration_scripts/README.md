# DLMS integration scripts

Wirepas DLMS example scripts for Wirepas NICs.

It provides example of scripts to listen to the DLMS traffic, to provision meters and to send on-demand query to the meters from a HES perspective and based on the wirepas dlms tool library.

Even if the example scripts show that the library is easy to use as is,
the library is not designed to be used as a HES.
This is designed to make sure the system NIC/meter is compatible and that their functionalities work.


## Installation

Installation of python library by selecting the wanted version of the dlms app examples in [github](https://github.com/wirepas/dlms-app/releases),
then downloading the library wheel (wirepas_dlms_tools-\<library version\>.whl) in the assets.

The following command should be used to install locally the library:

`pip install <name and path of the wheel .whl>`


## Documentation of the library

A documentation is also present in [github](https://github.com/wirepas/dlms-app/releases), by selecting the wanted version of the dlms app examples and downloading the corresponding html repository.
It contains the documentation of all the methods and functions in the python library that are accessible.


## Scripts

**Important**
'TODO' are left in the top of the scripts so that users know what needs to be modified.

The MQTT broker credentials, the meter credentials need to be changed in all the scripts.

As the keys and passwords need to be provided as hex string and the system title need to be provided as bytestring,
you might need to do convertion between hex strings and bytestring.

Consider using `bytes.fromhex(<your hex string>)` to convert a hex string to bytestring and,
`b"<your bytestring>".hex()` to convert the bytestring to a hex string.

### Active communication with meters

example_request.py script is provided to show how to send on-demand query to the meter.

It basically sends a request to the meter and waits for the response.

We can note that other queries can be requested to the meter and that its NIC server can be queried the same way.
Check the Meter class in the wirepas [DLMS tools library](https://github.com/wirepas/dlms-integration-tool) to see all the request methods that are available.

The script prints the main steps of the query in the console but more details information will be stored locally in the log file 'example_request.log'.

#### Execution

Execution of a script to test the communication of specific messages with a meter.

`python3 ./example_request.py`


#### Side notes

Another script `example_transparent_mode.py` shows how to communicate with the NIC and especially how to activate the transparent mode on the NIC. To execute the script, launch the following command:

`python3 ./example_transparent_mode.py`


### Listening to the DLMS traffic

A script is provided to listen to the data notifications sent by meters.

Each time a DLMS data notification message is received from a meter,
the message content is translated and logged with its source node id, the obis code,
the name of the profile data is retrieved from the obis code (e.g. “1.0.94.91.0.255“ => Instantaneous profile) and so on...,
so that the user knows what kind of message is received when it is logged.

The time duration of this script is infinite, and the script will continue until a "Enter" keyword is pressed.

The script prints the main steps of the listening in the console but more details information will be stored locally in the log file 'example_listen.log'.

#### Execution

Listen to meters for a certain time and give a summary of the periodic received messages.

`python3 ./example_listen.py`


### Provisioning the NICs

A script is provided to listen to the DLMS traffic for NIC status word and provision the NICs in need.
By default, in this script all the meters have a default ciphering configuration (the ciphering configuration of NICs that were flashed).
When a NIC status word message is received from a NIC that wants to be provisioned
the meter ciphering configuration of the node that sent the message is transmitted and
encrypted with the default configuration, so that it can communicate with its meter.
Once The NIC is provisioned, these new ciphering settings must be used in future exchanges.

The time duration of this script is infinite, and the script will continue until a "Enter" keyword is pressed.

The script prints the main steps of the provisioning in the console but more details information will be stored locally in the log file 'provisioning.log'.

#### Execution

Listen to meters for a certain time and give a summary of the periodic received messages.

`python3 ./provisioning.py`


### Integration tests

A script is provided to test all functionalities of the NIC and verify that they do work.
It includes requests to both the meter in pass-through and to the NIC server.


Basic use case is launching all the integration tests with the following command on a computer:

`python3 ./integration_tests.py --test_tags_to_execute all --filename ./integration_tests_output.log --config default_config.json`

The initial setup should be the following:
* 1 gateway uploading Wirepas messages to a MQTT broker.
* 1 sink connected to the gateway.
* 1 meter connected to 1 NIC running the DLMS app that is waiting to be provisionned (with meter credentials by default so that it cannot connect to the meter yet).
* The NIC should be able to communicate with the previously mentionned sink. And its logs should be stored somewhere.

The logs of the execution of the integration script will be stored in the log file input in the command.

default_config.json file must be customized according to your test setup.
* Edit MQTT settings for the meters
* Gateway and sink ids do not need to be fill if the tests are all executed
* Edit the meter configuration for the unprovisioned meters and the meter_configuration
* default_configuration must be aligned with the configuration set in dlms_app when the NIC was flashed. Note: If some keys are not needed, they can be removed from the configuration
* meter_configuration must be aligned with the configuration that the meter has now. Note: If some keys are not needed, they can be removed from the configuration",
* Edit the list of your meters to test with its meter configuration, system title and serial number in bytes / invocation counter if it is already incremented",
* System title (manufacturer_flag and meter_serial_number) needs to be set if tests do not start with a NIC server connection or provisioning tests
* Serial number input will be necessary to verify the NIC status word fields in the provisioning tests",
* Invocation counter needs to be set if tests do not start with a NIC provisioning",

Note:

* Tests that modify NIC/meter credentials might be dangerous as the NIC can be desynchronized from the meter after the tests. One way to know that there is a desynchronization is to check if the tests on secured associations (MR/US/FU) all fail after the credentials tests. To repare this, you will have to set back the keys of the meter in the NIC credentials informations, then change the keys back to their normal values if it's not the case. The expected NIC credentials will be logged, but the meter credentials can't be known or logged in this case as the desynchronisation can only happen if the meter do not respect the IS 15959 specifications.
* The tests are grouped by similarity under specific tags. For example: 'connection' tag group
tests that verify the connection of the backend to the NIC server and to the meter.
These tags can be used to run the integration tests on selected group of tests. Check with
`python3 ./integration_tests.py --help` to check for the different tags available if there is a need to not execute all the tests at once.
* Additional tests will be added later
