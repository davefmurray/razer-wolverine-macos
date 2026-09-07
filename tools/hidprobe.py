#!/usr/bin/env python3
"""Read-only IOHIDManager probe: list HID gamepads/joysticks and print raw input reports for N seconds.
Used to show that Razer Synapse's virtual 'Xbox 360 Controller' / 'VirtualJoystic' never emit data."""
import ctypes, ctypes.util, time, sys
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 10
CF = ctypes.CDLL(ctypes.util.find_library('CoreFoundation')); IOK = ctypes.CDLL(ctypes.util.find_library('IOKit'))
R=ctypes.c_void_p
CF.CFRunLoopGetCurrent.restype=R; CF.CFRunLoopRunInMode.restype=ctypes.c_int32; CF.CFRunLoopRunInMode.argtypes=[R,ctypes.c_double,ctypes.c_bool]
CF.CFStringCreateWithCString.restype=R; CF.CFStringCreateWithCString.argtypes=[R,ctypes.c_char_p,ctypes.c_uint32]
CF.CFStringGetCString.argtypes=[R,ctypes.c_char_p,ctypes.c_long,ctypes.c_uint32]; CF.CFStringGetCString.restype=ctypes.c_bool
CF.CFNumberGetValue.argtypes=[R,ctypes.c_int,ctypes.c_void_p]; CF.CFNumberGetValue.restype=ctypes.c_bool
CF.CFGetTypeID.restype=ctypes.c_ulong; CF.CFGetTypeID.argtypes=[R]; CF.CFStringGetTypeID.restype=ctypes.c_ulong
CF.CFSetGetCount.restype=ctypes.c_long; CF.CFSetGetCount.argtypes=[R]; CF.CFSetGetValues.argtypes=[R,ctypes.POINTER(R)]
mode=R.in_dll(CF,'kCFRunLoopDefaultMode')
IOK.IOHIDManagerCreate.restype=R; IOK.IOHIDManagerCreate.argtypes=[R,ctypes.c_uint32]
IOK.IOHIDManagerSetDeviceMatching.argtypes=[R,R]; IOK.IOHIDManagerOpen.argtypes=[R,ctypes.c_uint32]; IOK.IOHIDManagerOpen.restype=ctypes.c_int
IOK.IOHIDManagerCopyDevices.restype=R; IOK.IOHIDManagerCopyDevices.argtypes=[R]
IOK.IOHIDDeviceGetProperty.restype=R; IOK.IOHIDDeviceGetProperty.argtypes=[R,R]
IOK.IOHIDDeviceOpen.argtypes=[R,ctypes.c_uint32]; IOK.IOHIDDeviceOpen.restype=ctypes.c_int
IOK.IOHIDDeviceScheduleWithRunLoop.argtypes=[R,R,R]
CB=ctypes.CFUNCTYPE(None,ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_int,ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint8),ctypes.c_long)
IOK.IOHIDDeviceRegisterInputReportCallback.argtypes=[R,ctypes.POINTER(ctypes.c_uint8),ctypes.c_long,CB,ctypes.c_void_p]
def s(x): return CF.CFStringCreateWithCString(None,x.encode(),0x08000100)
def prop(dev,key):
    v=IOK.IOHIDDeviceGetProperty(dev,s(key))
    if not v: return None
    if CF.CFGetTypeID(v)==CF.CFStringGetTypeID():
        b=ctypes.create_string_buffer(256); CF.CFStringGetCString(v,b,256,0x08000100); return b.value.decode()
    n=ctypes.c_longlong(0); CF.CFNumberGetValue(v,4,ctypes.byref(n)); return n.value
mgr=IOK.IOHIDManagerCreate(None,0); IOK.IOHIDManagerSetDeviceMatching(mgr,None); IOK.IOHIDManagerOpen(mgr,0)
devs=IOK.IOHIDManagerCopyDevices(mgr); n=CF.CFSetGetCount(devs); arr=(R*n)(); CF.CFSetGetValues(devs,arr)
targets=[]
for d in arr:
    if prop(d,'PrimaryUsagePage')==1 and prop(d,'PrimaryUsage') in (4,5):
        print(f"HID game device: {prop(d,'Product')} vid=0x{prop(d,'VendorID'):04x} pid=0x{prop(d,'ProductID'):04x} maxIn={prop(d,'MaxInputReportSize')}")
        targets.append(d)
counts={}; cbs=[]
def cb(ctx,res,sender,typ,rid,rep,ln):
    counts[ctx]=counts.get(ctx,0)+1; print(f"[dev {ctx}] report id={rid} len={ln}: {bytes(rep[:ln]).hex(' ')}")
for i,d in enumerate(targets):
    print(f" open dev {i}: {IOK.IOHIDDeviceOpen(d,0):#x}")
    buf=(ctypes.c_uint8*64)(); c=CB(cb); cbs.append((buf,c))
    IOK.IOHIDDeviceRegisterInputReportCallback(d,buf,64,c,ctypes.c_void_p(i)); IOK.IOHIDDeviceScheduleWithRunLoop(d,CF.CFRunLoopGetCurrent(),mode)
print(f"Listening {SECS:.0f}s - move sticks / press buttons now...", flush=True)
t=time.time()
while time.time()-t<SECS: CF.CFRunLoopRunInMode(mode,0.25,False)
print("done; reports per device:", counts or "none")
