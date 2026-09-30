#!/usr/bin/env python3
"""Pair this PC with a watch over Bluetooth LE, as a phone does (lot S2).

    python firmware/tools/ble_pair.py --address AA:BB:CC:DD:EE:FF --status
    python firmware/tools/ble_pair.py --address AA:BB:CC:DD:EE:FF            # asks for the code
    python firmware/tools/ble_pair.py --address AA:BB:CC:DD:EE:FF --pin 91091
    python firmware/tools/ble_pair.py --address AA:BB:CC:DD:EE:FF --check
    python firmware/tools/ble_pair.py --address AA:BB:CC:DD:EE:FF --forget

The watch pairs with LE Secure Connections only, and shows its code on the glass ("PA" and
five digits): the code is typed here, which
authenticates the link. Windows then keeps the bond, and bleak uses it by itself: the
product image (release.conf) asks for that authenticated link on SMP and on Cobalt Link, so
ble_update.py and cobalt_link.py need it to reach a product. bleak's own pair() does only
"Just Works", which the watch refuses on purpose: this tool asks Windows for a passkey entry
directly (DeviceInformationCustomPairing, PROVIDE_PIN).

The watch keeps one bond and opens its pairing only when blank or reset (MODE and ALARM
held 10 s); three pairings that fail once the code is shown close it until a reset. So:
--forget forgets the watch on the Windows side only, and the watch still holds the bond of
this PC: a new pairing then needs a reset of the watch. Development images show a fixed code
(CONFIG_CB91AI_WATCH_DEBUG_PASSKEY in debug.conf, 91091), so that the bench pairs with no
one at the glass; a product draws its code at random at every pairing.

--check connects with bleak and reads the PAIR characteristic (C0B91B03-...), which needs an
authenticated link even on a development image: it reads only over the bond.

Requirements: bleak on Windows (its winrt packages come with it).
"""

import argparse
import asyncio
import sys

PAIR_UUID = "c0b91b03-256e-46f9-8d7b-d9c643908667"


async def _device(address):
    """The watch as Windows knows it, from its address (random static, as the watch's is)."""
    from winrt.windows.devices.bluetooth import BluetoothAddressType, BluetoothLEDevice
    value = int(address.replace(":", ""), 16)
    device = await BluetoothLEDevice.from_bluetooth_address_with_bluetooth_address_type_async(
        value, BluetoothAddressType.RANDOM)
    if device is None:
        device = await BluetoothLEDevice.from_bluetooth_address_async(value)
    if device is None:
        raise SystemExit(f"{address}: not known to Windows (is it advertising?)")
    return device


async def status(address):
    from winrt.windows.devices.enumeration import DevicePairingProtectionLevel
    device = await _device(address)
    pairing = device.device_information.pairing
    level = DevicePairingProtectionLevel(pairing.protection_level).name
    print(f"{address} ({device.name!r}): paired {pairing.is_paired}, can pair {pairing.can_pair}, "
          f"protection {level}")
    device.close()
    return pairing.is_paired


async def pair(address, pin):
    from winrt.windows.devices.enumeration import (DevicePairingKinds,
                                                   DevicePairingProtectionLevel,
                                                   DevicePairingResultStatus)
    device = await _device(address)
    pairing = device.device_information.pairing
    if pairing.is_paired:
        print(f"{address}: already paired with this PC")
        device.close()
        return True
    asked = []

    def on_request(_sender, args):
        # The watch shows its code; LE Secure Connections passkey entry on this side
        asked.append(args.pairing_kind)
        if args.pairing_kind != DevicePairingKinds.PROVIDE_PIN:
            return  # not accepted: Windows gives up this ceremony
        code = pin if pin is not None else input("Code on the watch (\"PA\" and five digits): ")
        args.accept_with_pin(code.strip().zfill(6))

    custom = pairing.custom
    token = custom.add_pairing_requested(on_request)
    try:
        result = await custom.pair_with_protection_level_async(
            DevicePairingKinds.PROVIDE_PIN,
            DevicePairingProtectionLevel.ENCRYPTION_AND_AUTHENTICATION)
    finally:
        custom.remove_pairing_requested(token)
        device.close()
    status_name = DevicePairingResultStatus(result.status).name
    print(f"{address}: {status_name} (ceremonies asked: "
          f"{', '.join(DevicePairingKinds(k).name for k in asked) or 'none'})")
    return result.status in (DevicePairingResultStatus.PAIRED,
                              DevicePairingResultStatus.ALREADY_PAIRED)


async def forget(address):
    from winrt.windows.devices.enumeration import DeviceUnpairingResultStatus
    device = await _device(address)
    result = await device.device_information.pairing.unpair_async()
    device.close()
    name = DeviceUnpairingResultStatus(result.status).name
    print(f"{address}: {name} on the Windows side; the watch keeps its bond until a reset")
    return result.status in (DeviceUnpairingResultStatus.UNPAIRED,
                             DeviceUnpairingResultStatus.ALREADY_UNPAIRED)


async def check(address):
    """PAIR reads only over an authenticated link: the bond at work."""
    from bleak import BleakClient, BleakScanner
    found = await BleakScanner.find_device_by_address(address, timeout=60.0)
    if found is None:
        raise SystemExit(f"{address}: not heard in 60 s")
    async with BleakClient(found, timeout=30.0) as client:
        try:
            value = await client.read_gatt_char(PAIR_UUID)
        except Exception as exc:  # noqa: BLE001 - refused: no bond, or not authenticated
            print(f"{address}: PAIR refused ({type(exc).__name__}: {exc})")
            return False
    print(f"{address}: PAIR read over the bond ({len(value)} byte(s)): the link is authenticated")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--address", required=True, help="Bluetooth address of the watch")
    parser.add_argument("--pin", help="the code shown on the watch, five digits (asked if missing)")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--status", action="store_true", help="paired or not, nothing changed")
    action.add_argument("--check", action="store_true",
                        help="read the protected PAIR characteristic over the bond")
    action.add_argument("--forget", action="store_true",
                        help="forget the watch on the Windows side (the watch keeps its bond)")
    args = parser.parse_args()
    if args.pin is not None and not (args.pin.isdigit() and len(args.pin) <= 6):
        parser.error("--pin: the digits shown on the watch")
    if args.status:
        asyncio.run(status(args.address))
        ok = True
    elif args.check:
        ok = asyncio.run(check(args.address))
    elif args.forget:
        ok = asyncio.run(forget(args.address))
    else:
        ok = asyncio.run(pair(args.address, args.pin))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
