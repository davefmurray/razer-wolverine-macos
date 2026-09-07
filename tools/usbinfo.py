#!/usr/bin/env python3
"""Read-only libusb probe: dump descriptors/endpoints of the Razer Wolverine V3 Pro (or any VID given)
and test whether userspace can claim interface 0. Uses Homebrew libusb via ctypes (no pyusb needed)."""
import ctypes, sys
VID = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x1532
for p in ('/opt/homebrew/opt/libusb/lib/libusb-1.0.dylib', '/usr/local/opt/libusb/lib/libusb-1.0.dylib'):
    try: lib = ctypes.CDLL(p); break
    except OSError: lib = None
if lib is None: sys.exit("libusb not found: brew install libusb")
class Desc(ctypes.Structure):
    _fields_=[('bLength',ctypes.c_uint8),('bDescriptorType',ctypes.c_uint8),('bcdUSB',ctypes.c_uint16),('bDeviceClass',ctypes.c_uint8),('bDeviceSubClass',ctypes.c_uint8),('bDeviceProtocol',ctypes.c_uint8),('bMaxPacketSize0',ctypes.c_uint8),('idVendor',ctypes.c_uint16),('idProduct',ctypes.c_uint16),('bcdDevice',ctypes.c_uint16),('iManufacturer',ctypes.c_uint8),('iProduct',ctypes.c_uint8),('iSerialNumber',ctypes.c_uint8),('bNumConfigurations',ctypes.c_uint8)]
class EP(ctypes.Structure):
    _fields_=[('bLength',ctypes.c_uint8),('bDescriptorType',ctypes.c_uint8),('bEndpointAddress',ctypes.c_uint8),('bmAttributes',ctypes.c_uint8),('wMaxPacketSize',ctypes.c_uint16),('bInterval',ctypes.c_uint8),('bRefresh',ctypes.c_uint8),('bSynchAddress',ctypes.c_uint8),('extra',ctypes.c_void_p),('extra_length',ctypes.c_int)]
class IfDesc(ctypes.Structure):
    _fields_=[('bLength',ctypes.c_uint8),('bDescriptorType',ctypes.c_uint8),('bInterfaceNumber',ctypes.c_uint8),('bAlternateSetting',ctypes.c_uint8),('bNumEndpoints',ctypes.c_uint8),('bInterfaceClass',ctypes.c_uint8),('bInterfaceSubClass',ctypes.c_uint8),('bInterfaceProtocol',ctypes.c_uint8),('iInterface',ctypes.c_uint8),('endpoint',ctypes.POINTER(EP)),('extra',ctypes.c_void_p),('extra_length',ctypes.c_int)]
class Iface(ctypes.Structure):
    _fields_=[('altsetting',ctypes.POINTER(IfDesc)),('num_altsetting',ctypes.c_int)]
class Cfg(ctypes.Structure):
    _fields_=[('bLength',ctypes.c_uint8),('bDescriptorType',ctypes.c_uint8),('wTotalLength',ctypes.c_uint16),('bNumInterfaces',ctypes.c_uint8),('bConfigurationValue',ctypes.c_uint8),('iConfiguration',ctypes.c_uint8),('bmAttributes',ctypes.c_uint8),('MaxPower',ctypes.c_uint8),('interface',ctypes.POINTER(Iface)),('extra',ctypes.c_void_p),('extra_length',ctypes.c_int)]
lib.libusb_init.argtypes=[ctypes.c_void_p]; lib.libusb_get_device_list.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))]
lib.libusb_get_device_descriptor.argtypes=[ctypes.c_void_p,ctypes.POINTER(Desc)]
lib.libusb_get_config_descriptor.argtypes=[ctypes.c_void_p,ctypes.c_uint8,ctypes.POINTER(ctypes.POINTER(Cfg))]
lib.libusb_open.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
lib.libusb_claim_interface.argtypes=[ctypes.c_void_p,ctypes.c_int]; lib.libusb_release_interface.argtypes=[ctypes.c_void_p,ctypes.c_int]
lib.libusb_get_configuration.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)]
lib.libusb_kernel_driver_active.argtypes=[ctypes.c_void_p,ctypes.c_int]
lib.libusb_strerror.restype=ctypes.c_char_p; lib.libusb_close.argtypes=[ctypes.c_void_p]
lib.libusb_init(None)
devs=ctypes.POINTER(ctypes.c_void_p)(); n=lib.libusb_get_device_list(None,ctypes.byref(devs))
found=0
for i in range(n):
    d=Desc(); lib.libusb_get_device_descriptor(devs[i],ctypes.byref(d))
    if d.idVendor!=VID: continue
    found+=1
    print(f"Found VID=0x{d.idVendor:04x} PID=0x{d.idProduct:04x} class={d.bDeviceClass:#x} sub={d.bDeviceSubClass:#x} proto={d.bDeviceProtocol:#x} bcdDevice={d.bcdDevice:#06x} nCfg={d.bNumConfigurations}")
    cfg=ctypes.POINTER(Cfg)(); r=lib.libusb_get_config_descriptor(devs[i],0,ctypes.byref(cfg))
    if r==0:
        c=cfg.contents; print(f" config value={c.bConfigurationValue} nIf={c.bNumInterfaces} attrs={c.bmAttributes:#x} maxPower={c.MaxPower*2}mA")
        for j in range(c.bNumInterfaces):
            ifc=c.interface[j]
            for a in range(ifc.num_altsetting):
                s=ifc.altsetting[a]
                print(f"  if#{s.bInterfaceNumber} alt{s.bAlternateSetting} class={s.bInterfaceClass:#x} sub={s.bInterfaceSubClass:#x} proto={s.bInterfaceProtocol:#x} nEP={s.bNumEndpoints}")
                for e in range(s.bNumEndpoints):
                    ep=s.endpoint[e]; print(f"     ep addr={ep.bEndpointAddress:#04x} attr={ep.bmAttributes:#x} maxpkt={ep.wMaxPacketSize} interval={ep.bInterval}")
    h=ctypes.c_void_p(); r=lib.libusb_open(devs[i],ctypes.byref(h)); print(" open:",r, lib.libusb_strerror(r).decode() if r else "ok")
    if r==0:
        cv=ctypes.c_int(-1); lib.libusb_get_configuration(h,ctypes.byref(cv)); print(" current config =",cv.value)
        print(" kernel_driver_active(0):",lib.libusb_kernel_driver_active(h,0),"(-5 = not supported on macOS = no driver)")
        r=lib.libusb_claim_interface(h,0); print(" claim if0:",r, lib.libusb_strerror(r).decode())
        if r==0: lib.libusb_release_interface(h,0)
        lib.libusb_close(h)
if not found: print(f"no device with VID 0x{VID:04x}")
