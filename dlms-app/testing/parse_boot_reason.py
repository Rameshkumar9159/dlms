#!/usr/bin/env python3

import argparse
import struct

class bcolors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    EXCEPTION = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'

BR_MAGIC=0x7c85
STACK_GUARD=0xDEADBEEF

MCU_RESET_POR = 0
MCU_RESET_INTENTIONAL = 1
MCU_RESET_ASSERT = 2
MCU_RESET_FAULT = 3
MCU_RESET_WDT = 4
MCU_RESET_UNKNOWN = 5
MCU_RESET_MODEM = 6
MCU_RESET_MODEM_INIT = 7
MCU_RESET_SOFT_WDT = 8
MCU_RESET_NMI = 9
MCU_RESET_HARD_FAULT = 10
MCU_RESET_BUS_FAULT = 11
MCU_RESET_USAGE_FAULT = 12
MCU_RESET_MEM_MANAGE_FAULT = 13
MCU_RESET_PIN = 14
MCU_RESET_CPU_LOCKUP = 15
MCU_RESET_SYSTEM_OFF_WAKEUP = 16
MCU_RESET_BROWNOUT = 17
MCU_RESET_SECURITY = 18
MCU_RESET_REQUEST = 19

reason_list = (
    "Normal power-on-reset startup",
    "Reboot requested as part of normal operation",
    "Reboot due to assert failure",
    "Reboot due to MCU fault (e.g. hard fault handler)",
    "Reboot due to HW watchdog",
    "Reboot due to unknown reason (e.g. direct call to NVIC_SystemReset())",
    "Reboot due to modem failure (NRF916x)",
    "Reboot due to modem failure at initialization (NRF916x)",
    "Reboot due to soft watchdog",
    "NMI Interrupt",
    "Hard Fault",
    "Bus Fault",
    "Usage Fault",
    "MemManage Fault",
    "Pin Reset",
    "CPU Lockup",
    "Wakeup from System-off (EM4 wakeup)",
    "Brown-out",
    "Security and secure debug control access port",
    "SysResetRequest, AIRCR.SYSRESETREQ, set e.g. by NVIC_SystemReset() or debugger interface."
)

last_will_list = (
    "Last will is not set ",
    "Blame Radio_changeChannel for my death ",
    "Blame scheduling part of Radio_sendPacket for my death ",
    "Blame sending part of Radio_sendPacket for my death ",
    "Blame scheduling part of Radio_receivePacket for my death ",
    "Blame receiving part of Radio_receivePacket for my death ",
    "Blame Radio_receiverOn for my death ",
    "Blame Radio_receiverOff for my death ",
    "Blame Radio_getTTOA for my death ",
    "Blame Radio_measureRssi for my death ",
    "Blame Radio_changePower for my death ",
    "Blame Radio_powerUp for my death ",
    "Blame radio modem init for my death ",
    "Blame energy management interrupt service routine for my death ",
    "Blame clock management interrupt service routine for my death ",
    "Blame USTIMER interrupt service routine for my death ",
    "Blame RTC interrupt service routine for my death ",
    "Blame Software interrupt service routine for my death ",
    "Blame Radio interrupt service routine for my death ",
    "Blame Thermal Shutdown for my death "
)

SCB_CFSR_BUSFAULTSR_Pos = 8
SCB_CFSR_BFARVALID_Pos = SCB_CFSR_BUSFAULTSR_Pos + 7
SCB_CFSR_BFARVALID_Msk = (1 << SCB_CFSR_BFARVALID_Pos)

def is_value_valid(rlist, r):

    return 0 <= r < len(rlist)

def get_description(rlist, reason):

    if 0 <= reason < len(rlist):
        return rlist[reason]
    return "invalid boot reason"

def print_boot_address_as_pc(boot_address):

    addr = "0x{a:08x}".format(a = boot_address)
    if boot_address:
        print(f"  Boot address   {bcolors.OKCYAN}{addr}{bcolors.ENDC}")
        print(f"  Use {bcolors.WARNING}addr2line {addr} -e <elf>{bcolors.ENDC} to find the faulty instruction")
    else:
        print(f"  Boot address   {addr}")

def is_cfsr_hfsr_valid(reason):

    return reason == MCU_RESET_HARD_FAULT or reason == MCU_RESET_BUS_FAULT or \
           reason == MCU_RESET_USAGE_FAULT or reason == MCU_RESET_MEM_MANAGE_FAULT

def is_bfar_valid(reason, info0):

    return (reason == MCU_RESET_HARD_FAULT or reason == MCU_RESET_BUS_FAULT) and (info0 & SCB_CFSR_BFARVALID_Msk);

def is_mmfar_valid(reason, info0):

    return (reason == MCU_RESET_HARD_FAULT or reason == MCU_RESET_MEM_MANAGE_FAULT) and (info0 & SCB_CFSR_MMARVALID_Msk)

def parse_data_32(data):

    #~ static const uint16_t m_magic_default = 0x7c85;

    #~ // breason_info0, breason_info1 and breason_info2 are for packing
    #~ // information from different boot reasons over the boot in
    #~ // .breason_sguard section that has limited space.
    #~ // DO NOT REPORT UNIONS USING SINGLE CBOR_ID TO BOOT DIAGNOSTICS!
    #~ // There must be own CBOR_ID for each union member for backend
    #~ // to separate different meanings and for documentation.
    #~ typedef union
    #~ {
        #~ // CFSR & HFSR
        #~ // bits  0 ..  7 MMFSR from CFSR
        #~ // bits  8 .. 15 BFSR from CFSR
        #~ // bits 16 .. 28 UFSR (13 lowest bits) from CFSR
        #~ // bit  29       VECTTBL from HFSR
        #~ // bit  30       FORCED from HFSR
        #~ // bit  31       DEBUGEVT from HFSR
        #~ uint32_t        cfsr_hfsr;
    #~ } breason_info0_t;

    #~ typedef union
    #~ {
        #~ uint32_t        bfar;
        #~ uint32_t        modem_reason;
    #~ } breason_info1_t;

    #~ typedef union
    #~ {
        #~ uint32_t        mmfar;
        #~ uint32_t        modem_program_counter;
    #~ } breason_info2_t;


    #~ /**
     #~ * \brief   Boot reason and stack guard.
     #~ *          This structure is to be protected using MPU, thus it shall be
     #~ *          located to proper alignment. As long as size of this struct is
     #~ *          32 bytes, proper alignment in both Cortex-M4 and Cortex-M33 is
     #~ *          32 bytes. (In case it grows in future it needs to double the
     #~ *          size. When size doubles, Cortex-M4 requires double size
     #~ *          alignment, while Cortex-M33 can always manage 32 byte
     #~ *          alignment and size multiply of 32 bytes.)
     #~ */
    #~ typedef struct
    #~ {
        #~ // Source filename hash
        #~ uint16_t        filenamehash;

        #~ // Line number
        #~ uint16_t        line_number;

        #~ // Boot reason
        #~ uint8_t         reason;

        #~ // Boot count
        #~ uint8_t         boot_count;

        #~ // Magic number. To indicate whether reason is valid.
        #~ uint16_t        magic;

        #~ // Task name hash
        #~ uint16_t        tasknamehash;

        #~ // Last will
        #~ uint16_t        last_will;

        #~ // Location in code (PC=program counter)
        #~ uint32_t        program_counter;

        #~ // Info 0 (e.g. CFSR & HFSR)
        #~ breason_info0_t info0;

        #~ // Info 1 (e.g. BFAR or Modem reason)
        #~ breason_info1_t info1;

        #~ // Info2 (e.g. MMFAR or Modem program counter)
        #~ breason_info2_t info2;

        #~ // Stack guard
        #~ uint32_t        stack_guard;
    #~ } breason_sguard_t;

    try:
        filenamehash, line_number, reason, boot_count, magic, tasknamehash, last_will, program_counter, info0, info1, info2, stack_guard = struct.unpack("<2H2B3H5I", data[0:32])
    except struct.error as e:
        logging.error(f"Error while unpacking: {str(e)}")
    else:
        if magic == BR_MAGIC:
            print(f"{bcolors.OKGREEN}  Valid magic{bcolors.ENDC}")
        else:
            print(data.hex())
            print(f"{bcolors.EXCEPTION}  Invalid magic{bcolors.ENDC}: {magic} (expected: {BR_MAGIC})")

        if stack_guard == STACK_GUARD:
            print(f"{bcolors.OKGREEN}  Valid stack guard{bcolors.ENDC}")
        else:
            print(f"{bcolors.EXCEPTION}  Invalid stack guard{bcolors.ENDC}: {stack_guard} (expected: {STACK_GUARD})")

        print(f"  File:line        {filenamehash:04x}:{line_number}")
        reason_string = get_description(reason_list, reason)
        print(f"  Reason           {bcolors.OKBLUE}{reason_string}{bcolors.ENDC} ({reason})")
        print(f"  Boot count       {boot_count}")
        print(f"  Task name        {tasknamehash:04x}")
        last_will_string = get_description(last_will_list, last_will)
        print(f"  Last will        {bcolors.OKBLUE}{last_will_string}{bcolors.ENDC} ({last_will})")
        print(f"  Program Counter  0x{program_counter:08X}")
        if program_counter:
            print(f"    Use {bcolors.WARNING}addr2line 0x{program_counter:08X} -e <elf>{bcolors.ENDC} to find the faulty instruction")
        if reason == MCU_RESET_MODEM:
            print(f"  Modem reason     0x{info1:08X}")
            print(f"  Modem PC         0x{info2:08X}")
        elif is_cfsr_hfsr_valid(reason):
            print(f"  CFSR & HFSR      0x{info0:08X}")
            if is_bfar_valid(reason, info0):
                print(f"  BFAR             0x{info1:08X}")
            elif is_mmfar_valid(reason, info0):
                print(f"  MMFAR            0x{info2:08X}")

def parse_data(data_string):

    try:
        data = bytes.fromhex(data_string)
    except ValueError as e:
        print(f"Failed to parse data '{data_string}': {e}")
        return

    l = len(data)
    if l == 32:
        parse_data_32(data)
    else:
        print(f"Failed to parse data: invalid length {l}")

def parse_file(file):

    with open(file, errors='ignore') as f:
        lines = [line.rstrip() for line in f]

    i = 0

    while i < len(lines):
        l = lines[i]
        i += 1
        #~ [TRC_BOOT][000002914] I: Hw: 16, Pp: 8, D:
        #~ 70 A1 F9 01 03 85 7C 62 82 CC 69 04 00 00 00 00
        if "[TRC_BOOT]" in l:
            print("line {i}: {l}".format(i = i, l = l))
            # if the data have been dumped
            if ", D" in l:
                # Today, the size of the boot reason structure is always 16 bytes
                data = lines[i] + lines[i + 1]
                parse_data(data)
                i += 2

if __name__ == "__main__":

    parser = argparse.ArgumentParser(fromfile_prefix_chars='@')
    parser.add_argument('--file',
                        help="DLMS log file")
    parser.add_argument('--data',
                        help="boot reason data")

    args = parser.parse_args()

    if args.file:
        parse_file(args.file)
    elif args.data:
        parse_data(args.data)
