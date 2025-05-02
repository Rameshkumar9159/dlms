# Wirepas DLMS reference application

> [!WARNING]
>
> _This example application is under development and is currently not completed. The purpose of current version is to demonstrate a basic meter reading as per the proposed architecture. It essentially allow to establish a communication with a meter over UART, read meter using DLMS client and generate a DLMS push message from the application. In this first version all meter credentials are hard coded and the reading is limited to only instant profile_
>

## Overview

This application is an example Wirepas optimized DLMS NIC application for Indian smart meters (compliant with IS15959 part 2).

The objective of the application is to provide a complete example of a DLMS integration into the NIC application in order to use the Wirepas network in its optimal way (packet size optimization, extensive usage of uplink rather than downlink, data randomization,...)

## Dependencies

### Sub-modules

This application relies on two other repositories managed as git submodules.

#### Wirepas SDK

This is the standard Wirepas SDK available as open source on [Github](https://github.com/wirepas/wm-sdk-subghz).

#### yahdlc - Yet Another HDLC

This library, available as open source on [Github](https://github.com/bang-olufsen/yahdlc), is used for NIC provisioning.

> [!WARNING]
>
> **This repository is not maintained by Wirepas and has its own License. Please consult it before using this application in your project and agree with the terms**
>

### Other dependency

#### Gurux DLMS library

It is used to interact between the NIC and the meter and also to generate the push messages from the NIC to the Head End System (HES). It is provided as a library.

## How to build

### Prerequisite steps

- Binaries that you have received from Wirepas (Stack and Bootloader) must be copied into [wirepas_binaries folder](wirepas_binaries/).
- Submodules must be initialized:

  ```shell
  make install
  ```

- In current version, there are two ways to set up meter settings:
  - add all settings in a meter config file based on [config file](dlms_app/config.mk) and compile the application by adding 'app_config=<config_file_prefix>' on the command line at build time
  - use NIC provisioning using the meter UART (refer to the ### NIC provisioning section)

> [!WARNING]
>
> **If some of the parameters are not set, the application will wait for NIC provisioning on the meter UART and not fully boot**
>

### Build the application

This application requires same tools as the Wirepas SDK. As a reminder Wirepas maintains a docker environment that has all the dependencies satisfied in an image available from Docker Hub.

Result of the build will be available in the build folder at the root of this project.

#### Build without docker

```shell
make
```

#### Build with a docker environment

```shell
docker run --rm -it -v $(pwd)/:$(pwd)/ -w $(pwd) wirepas/sdk-builder:v1.5 make
```

#### Build with a docker environment and a meter config file named 'meter_config.mk' and stored in [dlms_app](dlms_app/)

```shell
docker run --rm -it -v $(pwd)/:$(pwd)/ -w $(pwd) wirepas/sdk-builder:v1.5 make app_config=meter_config
```

## How to test

### Setup a gateway

How to setup a Gateway is not defined here but can be found [here](https://wirepas.freshdesk.com/support/solutions/articles/77000466081-how-to-set-up-a-wirepas-gateway-on-a-raspberry-pi-with-wirepas-prebuilt-image).

This repository also contains a second application called [sink_app](sink_app/) that can be used to prepare the Sinks for your gateways.
It contains the same network parameters as the [dlms_app](dlms_app/) and also the right memory mapping for loading large scratchpad.

#### Build the sink application without docker

```shell
make app_name=sink_app
```

#### Build the sink application with a docker environment

```shell
docker run --rm -it -v $(pwd)/:$(pwd)/ -w $(pwd) wirepas/sdk-builder:v1.5 make app_name=sink_app
```

### Test script

A test framework is available as a Python wheel artifact [here](https://github.com/wirepas/dlms-app/releases).

### NIC provisioning

Instructions for NIC provisoning are available in the following [README](dlms_app/preamble_app/scripts/README.md)

### NIC testing

Instructions for NIC testing are available in:

- [test_router_app README](test_router_app/README.md)
- [test_app README](test_app/README.md)

### End of Line testing
Instructions for End of Line testing (i.e NIC mounted in meter casing) are available in:
- [test_router_app README](test_router_app/README.md)
