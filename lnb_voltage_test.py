#!/usr/bin/env python3

import time
from pyftdi.ftdi import Ftdi, FtdiError


# OpenTuner-recognized MiniTiouner interface-A descriptions
TUNER_NAMES = (
    "MiniTiouner",
    "NIM tuner A",
    "MiniTiouner_Pro_TS2 A",
    "MiniTiouner A",
    "Minitiouner S A",
    "MiniTiouner-Express A",
)

# OpenTuner FT2232H GPIO setup
LOW_DIRECTION = 0xFF
LOW_VALUE = 0x00

HIGH_DIRECTION = 0xF1
HIGH_DEFAULT = 0x6F

# High-byte GPIO bits
LNB_ENABLE = 1 << 4
LNB_VSEL = 1 << 7


def find_tuner():
    print("Searching for FTDI devices...\n")

    devices = Ftdi.list_devices()

    if not devices:
        print("No FTDI devices found.")
        return None

    candidates = []

    for desc, interface_count in devices:
        name = desc.description or ""
        serial = desc.sn or "(no serial)"

        print(
            f"VID:PID {desc.vid:04x}:{desc.pid:04x}  "
            f"interfaces={interface_count}  "
            f"serial={serial}  "
            f"description='{name}'"
        )

        # Ftdi.list_devices() reports the number of interfaces, not an
        # interface number. A dual-channel MiniTiouner has two; GPIO is on A.
        if interface_count >= 1 and any(
            x.lower() in name.lower() for x in TUNER_NAMES
        ):
            candidates.append((desc, 1))

    print()

    if len(candidates) == 0:
        print("No recognized MiniTiouner interface A found.")
        return None

    if len(candidates) > 1:
        print("More than one MiniTiouner found.")
        print("For safety I will not choose one automatically.")
        return None

    return candidates[0]


def main():
    found = find_tuner()

    if not found:
        return

    desc, interface = found

    print("MiniTiouner detected:")
    print(f"  Description : {desc.description}")
    print(f"  Serial      : {desc.sn}")
    print(f"  Interface   : {interface}")
    print()

    ftdi = Ftdi()

    # Match the OpenTuner GPIO starting state:
    #
    # high byte = 0x6F
    # low byte  = 0x00
    #
    # direction:
    # high = 0xF1
    # low  = 0xFF

    direction = (HIGH_DIRECTION << 8) | LOW_DIRECTION
    initial = (HIGH_DEFAULT << 8) | LOW_VALUE

    print("Opening FT2232H interface A...")

    ftdi.open_mpsse(
        vendor=desc.vid,
        product=desc.pid,
        bus=desc.bus,
        address=desc.address,
        serial=desc.sn,
        interface=interface,
        direction=direction,
        initial=initial,
        frequency=200000,
    )

    print("FT2232H opened successfully.")
    print()

    high_value = HIGH_DEFAULT

    def write_high(value):
        """Equivalent of OpenTuner ftdi_gpio_write_highbyte()."""

        value &= 0xFF

        # MPSSE:
        # 0x82 = SET DATA BITS HIGH BYTE
        # byte 2 = GPIO values
        # byte 3 = GPIO directions

        data = bytes([0x82, value, HIGH_DIRECTION])

        written = ftdi.write_data(data)

        if written != len(data):
            raise IOError("FTDI write failed")

        time.sleep(0.05)

    def set_voltage(voltage):
        nonlocal high_value

        if voltage == 0:
            # OpenTuner:
            # ENABLE = 0
            # VSEL   = 0
            high_value &= ~LNB_ENABLE
            write_high(high_value)

            high_value &= ~LNB_VSEL
            write_high(high_value)

            print(f"LNB = OFF      GPIO high byte = 0x{high_value:02X}")

        elif voltage == 12:
            # Low-voltage / vertical selection
            # VSEL   = 0
            # ENABLE = 1
            high_value &= ~LNB_VSEL
            write_high(high_value)

            high_value |= LNB_ENABLE
            write_high(high_value)

            print(f"LNB = LOW      GPIO high byte = 0x{high_value:02X}")
            print("Expected approximately 12-13 V")

        elif voltage == 18:
            # High-voltage / horizontal selection
            # VSEL   = 1
            # ENABLE = 1
            high_value |= LNB_VSEL
            write_high(high_value)

            high_value |= LNB_ENABLE
            write_high(high_value)

            print(f"LNB = HIGH     GPIO high byte = 0x{high_value:02X}")
            print("Expected approximately 18 V")

    try:
        # Always begin safely OFF
        set_voltage(0)

        while True:
            print()
            print("--------------------------------")
            print("0   = LNB OFF")
            print("12  = LOW voltage (~12/13 V)")
            print("18  = HIGH voltage (~18 V)")
            print("q   = quit")
            print("--------------------------------")

            command = input("Command: ").strip().lower()

            if command == "0":
                set_voltage(0)
            elif command == "12":
                set_voltage(12)
            elif command == "18":
                set_voltage(18)
            elif command in ("q", "quit", "exit"):
                break
            else:
                print("Unknown command.")

    except KeyboardInterrupt:
        print("\nInterrupted.")

    finally:
        print("\nTurning LNB supply OFF...")

        try:
            set_voltage(0)
        except Exception:
            pass

        ftdi.close()

        print("Done.")


if __name__ == "__main__":
    try:
        main()
    except FtdiError as e:
        print()
        print("FTDI error:")
        print(e)
