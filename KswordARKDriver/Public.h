/*++

Module Name:

    public.h

Abstract:

    This module contains the common declarations shared by driver
    and user applications.

Environment:

    user and kernel

--*/

//
// Define an Interface Guid so that app can find the device and talk to it.
//

DEFINE_GUID (GUID_DEVINTERFACE_KswordARKDriver,
    0x457bf286,0x6314,0x4445,0x83,0xe8,0x41,0xd9,0xed,0xa4,0xb2,0xea);
// {457bf286-6314-4445-83e8-41d9eda4b2ea}
