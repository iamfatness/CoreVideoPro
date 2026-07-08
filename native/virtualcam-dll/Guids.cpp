// Emits storage for CLSID_CoreVideoVirtualCameraSource. Isolated in its own TU
// with <initguid.h> so only OUR guid gets a definition here (defining INITGUID
// in a file that also pulls in mfapi/mfidl would duplicate the SDK's guids).
#include <initguid.h>

#include "Guids.h"
