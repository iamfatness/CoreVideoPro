#pragma once

#include <guiddef.h>

// The media source CLSID. MUST match kMediaSourceClsid in
// VirtualCameraPublisher.cpp ({8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}) - the core
// passes this string to MFCreateVirtualCamera, and the Frame Server CoCreates
// this class from our DLL.
// {8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}
DEFINE_GUID(CLSID_CoreVideoVirtualCameraSource, 0x8b4b2c9e, 0x2c4a, 0x4e1d, 0x9c, 0x7a,
            0xcd, 0xef, 0x01, 0x23, 0x45, 0x67);
