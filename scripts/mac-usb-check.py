#!/usr/bin/env python3
"""Mac-side check for the tablet's USB modes (run on the Mac the tablet is plugged into; no extra tools needed).

  1. IOKit USB devices (ioreg -p IOUSB): vendor, product, name and the negotiated speed (1 full, 2 high = 480 Mb/s,
     3 SuperSpeed 5 Gb/s, 4 SuperSpeed+). The QcomDwc3 device stack (t970-usb) shows Apple's vendor 0x05ac; the CDC console
     (qcom-usb-debug) shows "Qualcomm XNU Debug".
  2. usbmuxd's own list (the ListDevices request on /var/run/usbmuxd, the same socket Finder and Xcode use): a tablet listed
     here is what Finder needs before it can ask for Trust.
  3. For each listed device, lockdownd through usbmux (port 62078): QueryType and the values an unpaired host may read
     (ProductType, ProductVersion, BuildVersion, ChipID, BoardId, HardwareModel, DeviceClass, UniqueDeviceID, ...). These are what
     Finder compares with MobileDevice's device map; nothing is paired or written.

usage: mac-usb-check.py [--watch SECONDS]
"""
import argparse
import plistlib
import socket
import struct
import subprocess
import time

SPEED = {0: 'low', 1: 'full 12M', 2: 'high 480M', 3: 'SuperSpeed 5G', 4: 'SuperSpeed+ 10G', 5: 'SuperSpeed+ 20G'}


def usb_devices():
    raw = subprocess.run(['ioreg', '-p', 'IOUSB', '-l', '-w0', '-a'], capture_output=True).stdout
    found = []

    def walk(node):
        if isinstance(node, dict):
            if 'idVendor' in node:
                found.append(node)
            for child in node.get('IORegistryEntryChildren', []):
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)
    if raw:
        walk(plistlib.loads(raw))
    return found


def exactly(s, n):
    data = b''
    while len(data) < n:
        part = s.recv(n - len(data))
        if not part:
            raise OSError('connection closed')
        data += part
    return data


def mux_request(s, message):
    body = plistlib.dumps(dict(message, ClientVersionString='t970-check', ProgName='t970-check'))
    s.sendall(struct.pack('<4I', 16 + len(body), 1, 8, 1) + body)   # length, version 1 (plist), type 8 (plist), tag
    length = struct.unpack('<I', exactly(s, 16)[:4])[0] - 16
    return plistlib.loads(exactly(s, length))


def mux_socket():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect('/var/run/usbmuxd')
    return s


def usbmux_devices():
    s = mux_socket()
    try:
        return mux_request(s, {'MessageType': 'ListDevices'}).get('DeviceList', [])
    finally:
        s.close()


def lockdown(device_id):
    """QueryType and GetValue (no key) on the device's lockdownd, as an unpaired host."""
    s = mux_socket()
    try:
        reply = mux_request(s, {'MessageType': 'Connect', 'DeviceID': device_id, 'PortNumber': socket.htons(62078)})
        if reply.get('Number', -1) != 0:
            return {'mux connect result': reply.get('Number')}
        def call(request):
            body = plistlib.dumps(dict(request, Label='t970-check'))
            s.sendall(struct.pack('>I', len(body)) + body)
            return plistlib.loads(exactly(s, struct.unpack('>I', exactly(s, 4))[0]))
        result = {'QueryType': call({'Request': 'QueryType'}).get('Type')}
        values = call({'Request': 'GetValue'})
        result.update(values.get('Value', {}) if isinstance(values.get('Value'), dict) else {'GetValue': values})
        return result
    finally:
        s.close()


def report():
    print(time.strftime('%H:%M:%S'))
    for d in usb_devices():
        name = d.get('USB Product Name') or d.get('kUSBProductString') or d.get('IORegistryEntryName')
        speed = d.get('Device Speed', d.get('USBSpeed'))
        mark = '  <- tablet?' if d.get('idVendor') == 0x05ac or 'XNU' in str(name) else ''
        print(f"  ioreg   {d.get('idVendor', 0):04x}:{d.get('idProduct', 0):04x} {name!s:30} speed {SPEED.get(speed, speed)}"
              f" serial {d.get('USB Serial Number', '-')}{mark}")
    try:
        devices = usbmux_devices()
    except OSError as error:
        print('  usbmuxd: no answer:', error)
        return
    if not devices:
        print('  usbmuxd: no devices')
    for d in devices:
        p = d.get('Properties', {})
        print(f"  usbmuxd {p.get('ConnectionType')} id {d.get('DeviceID')} udid {p.get('SerialNumber')} product {p.get('ProductID', 0):#x}"
              f" speed {p.get('ConnectionSpeed')}")
        if p.get('ConnectionType') != 'USB':
            continue
        try:
            info = lockdown(d.get('DeviceID'))
        except (OSError, ValueError) as error:
            print('    lockdownd: no answer:', error)
            continue
        keys = ('QueryType', 'DeviceName', 'DeviceClass', 'ProductType', 'ProductVersion', 'BuildVersion', 'HardwareModel', 'ChipID',
                'BoardId', 'UniqueDeviceID', 'UniqueChipID', 'ActivationState', 'PasswordProtected', 'GetValue', 'mux connect result')
        for k in keys:
            if k in info:
                print(f'    lockdownd {k}: {info[k]}')


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('--watch', type=float, default=0, help='repeat every N seconds')
    args = a.parse_args()
    while True:
        report()
        if not args.watch:
            break
        time.sleep(args.watch)


if __name__ == '__main__':
    main()
