/*++

Module Name:

    bugcheck_svga.c

Abstract:

    VMware SVGA-II detection, framebuffer/FIFO access, and crash-safe panel drawing.

--*/

#include "bugcheck_internal.h"
#include "bugcheck_font.h"

#include <ntstrsafe.h>
#include <stdarg.h>

#define KSW_SVGA_PCI_MAX_BUSES 256UL
#define KSW_SVGA_PCI_MAX_DEVICES 32UL
#define KSW_SVGA_PCI_MAX_FUNCTIONS 8UL

#define KSW_SVGA_PCI_BAR_IO 0x00000001UL
#define KSW_SVGA_PCI_BAR_MEM_TYPE_MASK 0x00000006UL
#define KSW_SVGA_PCI_BAR_MEM_TYPE_64 0x00000004UL
#define KSW_SVGA_PCI_BAR_IO_MASK 0xFFFFFFFCUL
#define KSW_SVGA_PCI_BAR_MEM_MASK 0xFFFFFFF0UL

#define KSW_SVGA_ID_INVALID 0xFFFFFFFFUL
#define KSW_SVGA_ID_0 0x90000000UL
#define KSW_SVGA_ID_1 0x90000001UL
#define KSW_SVGA_ID_2 0x90000002UL
#define KSW_SVGA_ID_3 0x90000003UL

#define KSW_SVGA_INDEX_PORT 0UL
#define KSW_SVGA_VALUE_PORT 1UL

#define KSW_SVGA_REG_ID 0UL
#define KSW_SVGA_REG_ENABLE 1UL
#define KSW_SVGA_REG_WIDTH 2UL
#define KSW_SVGA_REG_HEIGHT 3UL
#define KSW_SVGA_REG_DEPTH 6UL
#define KSW_SVGA_REG_BITS_PER_PIXEL 7UL
#define KSW_SVGA_REG_RED_MASK 9UL
#define KSW_SVGA_REG_GREEN_MASK 10UL
#define KSW_SVGA_REG_BLUE_MASK 11UL
#define KSW_SVGA_REG_BYTES_PER_LINE 12UL
#define KSW_SVGA_REG_FB_START 13UL
#define KSW_SVGA_REG_FB_OFFSET 14UL
#define KSW_SVGA_REG_VRAM_SIZE 15UL
#define KSW_SVGA_REG_FB_SIZE 16UL
#define KSW_SVGA_REG_CAPABILITIES 17UL
#define KSW_SVGA_REG_MEM_SIZE 19UL
#define KSW_SVGA_REG_CONFIG_DONE 20UL
#define KSW_SVGA_REG_SYNC 21UL
#define KSW_SVGA_REG_BUSY 22UL

#define KSW_SVGA_FIFO_MIN 0UL
#define KSW_SVGA_FIFO_MAX 1UL
#define KSW_SVGA_FIFO_NEXT_CMD 2UL
#define KSW_SVGA_FIFO_STOP 3UL
#define KSW_SVGA_FIFO_HEADER_DWORDS 4UL
#define KSW_SVGA_CMD_UPDATE 1UL

typedef struct _KSW_SVGA_PCI_BAR
{
    BOOLEAN Present;
    BOOLEAN IoSpace;
    BOOLEAN Is64Bit;
    ULONGLONG Address;
} KSW_SVGA_PCI_BAR, *PKSW_SVGA_PCI_BAR;

#if defined(_AMD64_) || defined(_M_AMD64)

static VOID
KswordARKSvgaWriteRegister(
    _In_ ULONG IoBase,
    _In_ ULONG Register,
    _In_ ULONG Value
    )
{
    WRITE_PORT_ULONG(
        (PULONG)(ULONG_PTR)(IoBase + KSW_SVGA_INDEX_PORT),
        Register);
    WRITE_PORT_ULONG(
        (PULONG)(ULONG_PTR)(IoBase + KSW_SVGA_VALUE_PORT),
        Value);
}

static ULONG
KswordARKSvgaReadRegister(
    _In_ ULONG IoBase,
    _In_ ULONG Register
    )
{
    WRITE_PORT_ULONG(
        (PULONG)(ULONG_PTR)(IoBase + KSW_SVGA_INDEX_PORT),
        Register);
    return READ_PORT_ULONG(
        (PULONG)(ULONG_PTR)(IoBase + KSW_SVGA_VALUE_PORT));
}

static ULONG
KswordARKSvgaProbeId(
    _In_ ULONG IoBase
    )
{
    static const ULONG ids[] = {
        KSW_SVGA_ID_3,
        KSW_SVGA_ID_2,
        KSW_SVGA_ID_1,
        KSW_SVGA_ID_0
    };
    ULONG index;

    for (index = 0; index < RTL_NUMBER_OF(ids); ++index) {
        KswordARKSvgaWriteRegister(IoBase, KSW_SVGA_REG_ID, ids[index]);
        if (KswordARKSvgaReadRegister(IoBase, KSW_SVGA_REG_ID) == ids[index]) {
            return ids[index];
        }
    }
    return KSW_SVGA_ID_INVALID;
}

static VOID
KswordARKSvgaParseBar(
    _In_ ULONG RawBar,
    _In_ ULONG NextRawBar,
    _Out_ PKSW_SVGA_PCI_BAR Bar
    )
{
    RtlZeroMemory(Bar, sizeof(*Bar));
    if (RawBar == 0 || RawBar == 0xFFFFFFFFUL) {
        return;
    }

    Bar->Present = TRUE;
    if ((RawBar & KSW_SVGA_PCI_BAR_IO) != 0) {
        Bar->IoSpace = TRUE;
        Bar->Address = RawBar & KSW_SVGA_PCI_BAR_IO_MASK;
        return;
    }

    Bar->Is64Bit =
        ((RawBar & KSW_SVGA_PCI_BAR_MEM_TYPE_MASK) == KSW_SVGA_PCI_BAR_MEM_TYPE_64)
            ? TRUE
            : FALSE;
    Bar->Address = RawBar & KSW_SVGA_PCI_BAR_MEM_MASK;
    if (Bar->Is64Bit) {
        Bar->Address |= ((ULONGLONG)NextRawBar << 32);
    }
}

static BOOLEAN
KswordARKSvgaFindVmwareDisplay(
    _Out_ PPCI_COMMON_CONFIG Config,
    _Out_writes_(KSWORD_ARK_PCI_BAR_COUNT) PKSW_SVGA_PCI_BAR Bars,
    _Out_ PULONG BusOut,
    _Out_ PULONG DeviceOut,
    _Out_ PULONG FunctionOut
    )
{
    ULONG bus;
    ULONG device;
    ULONG function;
    ULONG bytesRead;
    ULONG barIndex;
    PCI_SLOT_NUMBER slot;
    PCI_COMMON_CONFIG config;

    RtlZeroMemory(Config, sizeof(*Config));
    RtlZeroMemory(Bars, sizeof(KSW_SVGA_PCI_BAR) * KSWORD_ARK_PCI_BAR_COUNT);

    for (bus = 0; bus < KSW_SVGA_PCI_MAX_BUSES; ++bus) {
        for (device = 0; device < KSW_SVGA_PCI_MAX_DEVICES; ++device) {
            for (function = 0; function < KSW_SVGA_PCI_MAX_FUNCTIONS; ++function) {
                RtlZeroMemory(&slot, sizeof(slot));
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;
                RtlZeroMemory(&config, sizeof(config));

#pragma warning(push)
#pragma warning(disable: 4996)
                bytesRead = HalGetBusDataByOffset(
                    PCIConfiguration,
                    bus,
                    slot.u.AsULONG,
                    &config,
                    0,
                    sizeof(config));
#pragma warning(pop)
                if (bytesRead < PCI_COMMON_HDR_LENGTH ||
                    config.VendorID != KSWORD_ARK_VMWARE_VENDOR_ID ||
                    config.BaseClass != 0x03 ||
                    (config.HeaderType & 0x7FUL) != PCI_DEVICE_TYPE) {
                    continue;
                }

                *Config = config;
                for (barIndex = 0; barIndex < KSWORD_ARK_PCI_BAR_COUNT; ++barIndex) {
                    const ULONG nextRawBar = (barIndex + 1UL) < KSWORD_ARK_PCI_BAR_COUNT
                        ? config.u.type0.BaseAddresses[barIndex + 1UL]
                        : 0;
                    KswordARKSvgaParseBar(
                        config.u.type0.BaseAddresses[barIndex],
                        nextRawBar,
                        &Bars[barIndex]);
                }

                *BusOut = bus;
                *DeviceOut = device;
                *FunctionOut = function;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOLEAN
KswordARKSvgaValidateGeometry(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    ULONGLONG visibleBytes;
    ULONG bytesPerPixel;

    if (Context->Width == 0 || Context->Height == 0 ||
        Context->Width > 16384UL || Context->Height > 16384UL ||
        (Context->Bpp != 15UL && Context->Bpp != 16UL &&
         Context->Bpp != 24UL && Context->Bpp != 32UL)) {
        return FALSE;
    }

    bytesPerPixel = (Context->Bpp + 7UL) / 8UL;
    if (bytesPerPixel == 0 ||
        Context->Pitch < Context->Width * bytesPerPixel ||
        Context->Pitch > (1024UL * 1024UL)) {
        return FALSE;
    }

    visibleBytes = (ULONGLONG)Context->Pitch * Context->Height;
    if (visibleBytes == 0 || visibleBytes > (256ULL * 1024ULL * 1024ULL)) {
        return FALSE;
    }
    if (Context->FbSize != 0 &&
        ((ULONGLONG)Context->FbOffset >= Context->FbSize ||
         visibleBytes + Context->FbOffset > Context->FbSize)) {
        return FALSE;
    }

    Context->FramebufferLength = (SIZE_T)visibleBytes;
    return TRUE;
}

NTSTATUS
KswordARKBugcheckSvgaInitialize(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    PCI_COMMON_CONFIG config;
    KSW_SVGA_PCI_BAR bars[KSWORD_ARK_PCI_BAR_COUNT];
    PKSW_SVGA_PCI_BAR fifoBar;
    ULONG bus;
    ULONG device;
    ULONG function;
    ULONG svgaId;
    ULONG fifoBytes;
    PHYSICAL_ADDRESS physical;
    PVOID mapped;

    if (Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Context, sizeof(*Context));
    RtlZeroMemory(&config, sizeof(config));
    RtlZeroMemory(bars, sizeof(bars));

    if (!KswordARKSvgaFindVmwareDisplay(
            &config,
            bars,
            &bus,
            &device,
            &function)) {
        return STATUS_NOT_SUPPORTED;
    }

    // Require the VMware SVGA-II port/framebuffer BAR layout before touching it.
    if (!bars[0].Present || !bars[0].IoSpace ||
        !bars[1].Present || bars[1].IoSpace ||
        bars[0].Address == 0 || bars[0].Address > MAXULONG) {
        return STATUS_NOT_SUPPORTED;
    }

    svgaId = KswordARKSvgaProbeId((ULONG)bars[0].Address);
    if (svgaId == KSW_SVGA_ID_INVALID) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    Context->Found = TRUE;
    Context->Bus = bus;
    Context->Device = device;
    Context->Function = function;
    Context->VendorId = config.VendorID;
    Context->DeviceId = config.DeviceID;
    Context->IoBase = (ULONG)bars[0].Address;
    Context->Width = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_WIDTH);
    Context->Height = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_HEIGHT);
    Context->Depth = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_DEPTH);
    Context->Bpp = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_BITS_PER_PIXEL);
    Context->Pitch = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_BYTES_PER_LINE);
    Context->FbOffset = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_FB_OFFSET);
    Context->FbSize = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_FB_SIZE);
    Context->VramSize = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_VRAM_SIZE);
    Context->RedMask = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_RED_MASK);
    Context->GreenMask = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_GREEN_MASK);
    Context->BlueMask = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_BLUE_MASK);
    Context->Capabilities = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_CAPABILITIES);

    if (!KswordARKSvgaValidateGeometry(Context) ||
        bars[1].Address > (ULONGLONG)MAXLONGLONG - Context->FbOffset) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    physical.QuadPart = (LONGLONG)(bars[1].Address + Context->FbOffset);
    mapped = MmMapIoSpace(physical, Context->FramebufferLength, MmNonCached);
    if (mapped == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Context->FramebufferPhysical = physical;
    Context->Framebuffer = (volatile UCHAR*)mapped;
    Context->Mapped = TRUE;

    fifoBar = bars[1].Is64Bit ? &bars[3] : &bars[2];
    if (fifoBar->Present && !fifoBar->IoSpace) {
        fifoBytes = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_MEM_SIZE);
        if (fifoBytes >= 4096UL && fifoBytes <= (16UL * 1024UL * 1024UL)) {
            physical.QuadPart = (LONGLONG)fifoBar->Address;
            mapped = MmMapIoSpace(physical, fifoBytes, MmNonCached);
            if (mapped != NULL) {
                Context->FifoPhysical = physical;
                Context->FifoLength = fifoBytes;
                Context->Fifo = (volatile ULONG*)mapped;
                Context->FifoMapped = TRUE;
            }
        }
    }

    return STATUS_SUCCESS;
}

VOID
KswordARKBugcheckSvgaShutdown(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    if (Context == NULL) {
        return;
    }
    if (Context->FifoMapped && Context->Fifo != NULL) {
        MmUnmapIoSpace((PVOID)Context->Fifo, Context->FifoLength);
    }
    if (Context->Mapped && Context->Framebuffer != NULL) {
        MmUnmapIoSpace((PVOID)Context->Framebuffer, Context->FramebufferLength);
    }
    RtlZeroMemory(Context, sizeof(*Context));
}

static VOID
KswordARKSvgaPortSyncNoLog(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    ULONG spin;

    if (Context == NULL || Context->IoBase == 0) {
        return;
    }
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_SYNC, 1UL);
    for (spin = 0; spin < 1000000UL; ++spin) {
        if (KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_BUSY) == 0) {
            break;
        }
        KeStallExecutionProcessor(1);
    }
}

VOID
KswordARKBugcheckSvgaModeSetNoLog(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    ULONG configDone;

    if (Context == NULL || !Context->Mapped ||
        Context->Framebuffer == NULL || Context->IoBase == 0) {
        return;
    }

    configDone = KswordARKSvgaReadRegister(Context->IoBase, KSW_SVGA_REG_CONFIG_DONE);
    if (configDone == 0) {
        configDone = 1;
    }
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_ENABLE, 0UL);
    KeStallExecutionProcessor(1000);
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_WIDTH, Context->Width);
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_HEIGHT, Context->Height);
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_BITS_PER_PIXEL, Context->Bpp);
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_ENABLE, 1UL);
    KswordARKSvgaWriteRegister(Context->IoBase, KSW_SVGA_REG_CONFIG_DONE, configDone);
    KeStallExecutionProcessor(1000);
    KswordARKSvgaPortSyncNoLog(Context);
}

static BOOLEAN
KswordARKSvgaFifoUpdateNoLog(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_ ULONG Width,
    _In_ ULONG Height
    )
{
    volatile ULONG* fifo;
    ULONG min;
    ULONG max;
    ULONG next;
    ULONG stop;
    ULONG freeBytes;
    ULONG values[5];
    ULONG index;

    if (Context == NULL || !Context->FifoMapped || Context->Fifo == NULL ||
        Context->FifoLength < (KSW_SVGA_FIFO_HEADER_DWORDS * sizeof(ULONG))) {
        return FALSE;
    }

    fifo = Context->Fifo;
    min = fifo[KSW_SVGA_FIFO_MIN];
    max = fifo[KSW_SVGA_FIFO_MAX];
    next = fifo[KSW_SVGA_FIFO_NEXT_CMD];
    stop = fifo[KSW_SVGA_FIFO_STOP];
    if (min < KSW_SVGA_FIFO_HEADER_DWORDS * sizeof(ULONG) ||
        max > Context->FifoLength || min >= max ||
        next < min || next >= max || stop < min || stop >= max ||
        ((min | max | next | stop) & 3UL) != 0) {
        return FALSE;
    }

    if (next >= stop) {
        freeBytes = (max - next) + (stop - min);
    } else {
        freeBytes = stop - next;
    }
    if (freeBytes <= sizeof(values)) {
        return FALSE;
    }

    values[0] = KSW_SVGA_CMD_UPDATE;
    values[1] = X;
    values[2] = Y;
    values[3] = Width;
    values[4] = Height;
    for (index = 0; index < RTL_NUMBER_OF(values); ++index) {
        fifo[next / sizeof(ULONG)] = values[index];
        next += sizeof(ULONG);
        if (next == max) {
            next = min;
        }
    }
    KeMemoryBarrier();
    fifo[KSW_SVGA_FIFO_NEXT_CMD] = next;
    KeMemoryBarrier();
    KswordARKSvgaPortSyncNoLog(Context);
    return TRUE;
}

static ULONG
KswordARKSvgaPackChannel(
    _In_ UCHAR Value,
    _In_ ULONG Mask
    )
{
    ULONG shift = 0;
    ULONG bits = 0;
    ULONG work = Mask;
    ULONG maximum;

    if (Mask == 0) {
        return 0;
    }
    while ((work & 1UL) == 0) {
        ++shift;
        work >>= 1;
    }
    while ((work & 1UL) != 0) {
        ++bits;
        work >>= 1;
    }
    maximum = bits >= 31 ? MAXULONG : ((1UL << bits) - 1UL);
    return ((((ULONG)Value * maximum) + 127UL) / 255UL) << shift;
}

static ULONG
KswordARKSvgaPixelFromRgb(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ UCHAR Red,
    _In_ UCHAR Green,
    _In_ UCHAR Blue
    )
{
    ULONG redMask = Context->RedMask;
    ULONG greenMask = Context->GreenMask;
    ULONG blueMask = Context->BlueMask;

    if (redMask == 0 || greenMask == 0 || blueMask == 0) {
        if (Context->Bpp == 15) {
            redMask = 0x7C00UL;
            greenMask = 0x03E0UL;
            blueMask = 0x001FUL;
        } else if (Context->Bpp == 16) {
            redMask = 0xF800UL;
            greenMask = 0x07E0UL;
            blueMask = 0x001FUL;
        } else {
            redMask = 0x00FF0000UL;
            greenMask = 0x0000FF00UL;
            blueMask = 0x000000FFUL;
        }
    }

    return KswordARKSvgaPackChannel(Red, redMask) |
           KswordARKSvgaPackChannel(Green, greenMask) |
           KswordARKSvgaPackChannel(Blue, blueMask);
}

static VOID
KswordARKSvgaWritePixel(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_ ULONG Pixel
    )
{
    volatile UCHAR* destination;
    ULONG bytesPerPixel;

    if (Context == NULL || !Context->Mapped || Context->Framebuffer == NULL ||
        X >= Context->Width || Y >= Context->Height) {
        return;
    }
    bytesPerPixel = (Context->Bpp + 7UL) / 8UL;
    destination = Context->Framebuffer + ((SIZE_T)Y * Context->Pitch) +
                  ((SIZE_T)X * bytesPerPixel);
    if (bytesPerPixel == 4) {
        *(volatile ULONG*)destination = Pixel;
    } else if (bytesPerPixel == 3) {
        destination[0] = (UCHAR)(Pixel & 0xFF);
        destination[1] = (UCHAR)((Pixel >> 8) & 0xFF);
        destination[2] = (UCHAR)((Pixel >> 16) & 0xFF);
    } else if (bytesPerPixel == 2) {
        *(volatile USHORT*)destination = (USHORT)Pixel;
    }
}

static VOID
KswordARKSvgaFillRect(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Right,
    _In_ ULONG Bottom,
    _In_ ULONG Pixel
    )
{
    ULONG x;
    ULONG y;

    if (Context == NULL || Left >= Context->Width || Top >= Context->Height) {
        return;
    }
    if (Right > Context->Width) {
        Right = Context->Width;
    }
    if (Bottom > Context->Height) {
        Bottom = Context->Height;
    }
    for (y = Top; y < Bottom; ++y) {
        for (x = Left; x < Right; ++x) {
            KswordARKSvgaWritePixel(Context, x, y, Pixel);
        }
    }
}

static VOID
KswordARKSvgaDrawCharacter(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_ CHAR Character,
    _In_ ULONG Color,
    _In_ ULONG Scale
    )
{
    ULONG row;
    ULONG column;
    ULONG dx;
    ULONG dy;
    UCHAR bits;
    ULONG glyphIndex;

    if ((UCHAR)Character < KSWORD_ARK_BUGCHECK_FONT_FIRST ||
        (UCHAR)Character > KSWORD_ARK_BUGCHECK_FONT_LAST) {
        Character = '?';
    }
    if (Scale == 0) {
        Scale = 1;
    }

    glyphIndex = (UCHAR)Character - KSWORD_ARK_BUGCHECK_FONT_FIRST;
    for (row = 0; row < KSWORD_ARK_BUGCHECK_FONT_HEIGHT; ++row) {
        bits = g_KswordArkBugcheckFont8x12[glyphIndex][row];
        for (column = 0; column < KSWORD_ARK_BUGCHECK_FONT_WIDTH; ++column) {
            if ((bits & (0x80U >> column)) == 0) {
                continue;
            }
            for (dy = 0; dy < Scale; ++dy) {
                for (dx = 0; dx < Scale; ++dx) {
                    KswordARKSvgaWritePixel(
                        Context,
                        X + column * Scale + dx,
                        Y + row * Scale + dy,
                        Color);
                }
            }
        }
    }
}

static VOID
KswordARKSvgaDrawText(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_z_ PCSTR Text,
    _In_ ULONG Color,
    _In_ ULONG Scale
    )
{
    ULONG cursor = X;

    if (Text == NULL) {
        return;
    }
    while (*Text != '\0') {
        KswordARKSvgaDrawCharacter(Context, cursor, Y, *Text, Color, Scale);
        cursor += (KSWORD_ARK_BUGCHECK_FONT_WIDTH + 1UL) * Scale;
        if (cursor >= Context->Width) {
            break;
        }
        ++Text;
    }
}

static VOID
KswordARKSvgaDrawWrappedText(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_ ULONG Width,
    _In_z_ PCSTR Text,
    _In_ ULONG Color
    )
{
    CHAR line[KSWORD_ARK_BUGCHECK_PANEL_LINE_CHARS];
    ULONG maxChars;
    ULONG lineLength = 0;
    PCSTR cursor = Text;

    if (Text == NULL || Width < (KSWORD_ARK_BUGCHECK_FONT_WIDTH + 1UL)) {
        return;
    }
    maxChars = Width / (KSWORD_ARK_BUGCHECK_FONT_WIDTH + 1UL);
    if (maxChars >= RTL_NUMBER_OF(line)) {
        maxChars = RTL_NUMBER_OF(line) - 1UL;
    }
    line[0] = '\0';

    while (*cursor != '\0') {
        PCSTR wordStart;
        ULONG wordLength = 0;
        ULONG index;

        while (*cursor == ' ') {
            ++cursor;
        }
        wordStart = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            ++cursor;
            ++wordLength;
        }
        if (wordLength == 0) {
            break;
        }
        if (lineLength != 0 && lineLength + 1UL + wordLength > maxChars) {
            KswordARKSvgaDrawText(Context, X, Y, line, Color, 1);
            Y += KSWORD_ARK_BUGCHECK_FONT_HEIGHT + 3UL;
            lineLength = 0;
        }
        if (lineLength != 0) {
            line[lineLength++] = ' ';
        }
        for (index = 0;
             index < wordLength && lineLength < RTL_NUMBER_OF(line) - 1UL;
             ++index) {
            line[lineLength++] = wordStart[index];
        }
        line[lineLength] = '\0';
    }
    if (lineLength != 0) {
        KswordARKSvgaDrawText(Context, X, Y, line, Color, 1);
    }
}

static VOID
KswordARKSvgaDrawPanelLine(
    _In_ PKSWORD_ARK_SVGA_CONTEXT Context,
    _In_ ULONG X,
    _Inout_ PULONG Y,
    _In_ ULONG Color,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    CHAR line[KSWORD_ARK_BUGCHECK_PANEL_LINE_CHARS];
    va_list arguments;

    line[0] = '\0';
    va_start(arguments, Format);
    (VOID)RtlStringCbVPrintfA(line, sizeof(line), Format, arguments);
    va_end(arguments);
    line[RTL_NUMBER_OF(line) - 1] = '\0';
    KswordARKSvgaDrawText(Context, X, *Y, line, Color, 1);
    *Y += KSWORD_ARK_BUGCHECK_FONT_HEIGHT + 3UL;
}

static BOOLEAN
KswordARKSvgaDrawBitmap(
    _In_ PKSWORD_ARK_BUGCHECK_STATE State,
    _In_ ULONG DestinationX,
    _In_ ULONG DestinationY,
    _Out_ PULONG DrawnWidth,
    _Out_ PULONG DrawnHeight
    )
{
    ULONG width;
    ULONG height;
    ULONG x;
    ULONG y;
    const UCHAR* source;
    ULONG pixel;

    *DrawnWidth = 0;
    *DrawnHeight = 0;
    if (InterlockedCompareExchange(&State->Bitmap.Valid, 1, 1) == 0 ||
        DestinationX >= State->Svga.Width || DestinationY >= State->Svga.Height) {
        return FALSE;
    }

    width = State->Bitmap.Width;
    height = State->Bitmap.Height;
    if (DestinationX + width > State->Svga.Width) {
        width = State->Svga.Width - DestinationX;
    }
    if (DestinationY + height > State->Svga.Height) {
        height = State->Svga.Height - DestinationY;
    }

    for (y = 0; y < height; ++y) {
        source = g_KswordArkBugcheckBitmapPixels + ((SIZE_T)y * State->Bitmap.Stride);
        for (x = 0; x < width; ++x) {
            pixel = KswordARKSvgaPixelFromRgb(
                &State->Svga,
                source[(SIZE_T)x * 4UL + 2UL],
                source[(SIZE_T)x * 4UL + 1UL],
                source[(SIZE_T)x * 4UL + 0UL]);
            KswordARKSvgaWritePixel(
                &State->Svga,
                DestinationX + x,
                DestinationY + y,
                pixel);
        }
    }
    *DrawnWidth = width;
    *DrawnHeight = height;
    return TRUE;
}

VOID
KswordARKBugcheckSvgaDrawPanelNoLog(
    _Inout_ PKSWORD_ARK_BUGCHECK_STATE State
    )
{
    PKSWORD_ARK_SVGA_CONTEXT svga;
    PKSWORD_ARK_BUGCHECK_DIAGNOSTICS diag;
    ULONG white;
    ULONG brand;
    ULONG logoX = 24;
    ULONG logoY = 20;
    ULONG logoWidth = 0;
    ULONG logoHeight = 0;
    ULONG textX;
    ULONG textY;
    ULONG y;
    ULONG fallbackRight;
    ULONG fallbackBottom;

    if (State == NULL ||
        InterlockedCompareExchange(&State->Active, 1, 1) == 0) {
        return;
    }
    svga = &State->Svga;
    diag = &State->Diagnostics;
    if (!svga->Mapped || svga->Framebuffer == NULL ||
        svga->Width == 0 || svga->Height == 0) {
        return;
    }

    white = KswordARKSvgaPixelFromRgb(svga, 255, 255, 255);
    brand = KswordARKSvgaPixelFromRgb(
        svga,
        (UCHAR)((State->Bitmap.BrandColorRgb >> 16) & 0xFF),
        (UCHAR)((State->Bitmap.BrandColorRgb >> 8) & 0xFF),
        (UCHAR)(State->Bitmap.BrandColorRgb & 0xFF));
    KswordARKSvgaFillRect(svga, 0, 0, svga->Width, svga->Height, white);

    if (!KswordARKSvgaDrawBitmap(
            State,
            logoX,
            logoY,
            &logoWidth,
            &logoHeight)) {
        logoWidth = 300;
        logoHeight = 76;
        fallbackRight = logoX + logoWidth;
        fallbackBottom = logoY + logoHeight;
        if (fallbackRight > svga->Width) {
            fallbackRight = svga->Width;
            logoWidth = fallbackRight > logoX ? fallbackRight - logoX : 0;
        }
        if (fallbackBottom > svga->Height) {
            fallbackBottom = svga->Height;
            logoHeight = fallbackBottom > logoY ? fallbackBottom - logoY : 0;
        }
        KswordARKSvgaFillRect(
            svga,
            logoX,
            logoY,
            fallbackRight,
            fallbackBottom,
            brand);
        KswordARKSvgaDrawText(
            svga,
            logoX + 16,
            logoY + 25,
            "KswordARK",
            white,
            2);
    }

    textX = logoX + logoWidth + 24;
    if (svga->Width <= 344 || textX > svga->Width - 320) {
        textX = 24;
        textY = logoY + logoHeight + 10;
    } else {
        textY = logoY + 18;
    }
    KswordARKSvgaDrawText(
        svga,
        textX,
        textY,
        "KSWORDARK VMWARE BUGCHECK PANEL",
        brand,
        1);
    textY += 22;
    if (textX + 24 < svga->Width) {
        KswordARKSvgaDrawWrappedText(
            svga,
            textX,
            textY,
            svga->Width - textX - 24,
            KswordARKBugcheckVerdictText(diag->CandidateClass),
            brand);
    }

    y = logoY + logoHeight + 24;
    if (y < 250) {
        y = 250;
    }
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "STOP CODE : 0x%08lX", diag->BugCheckCode);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "STOP NAME : %s", KswordARKBugcheckName(diag->BugCheckCode));
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "PARAM1    : 0x%p", (PVOID)diag->Parameter1);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "PARAM2    : 0x%p", (PVOID)diag->Parameter2);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "PARAM3    : 0x%p", (PVOID)diag->Parameter3);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "PARAM4    : 0x%p", (PVOID)diag->Parameter4);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "FAULT IP  : param%lu 0x%p (%s)", diag->FaultParameter, (PVOID)diag->FaultAddress, diag->FaultMeaning);
    y += 6;
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "REASON    : %s (%lu)", KswordARKBugcheckReasonText(diag->LastReason), diag->LastReason);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "DUMP TYPE : %s (%lu)", KswordARKBugcheckDumpTypeText(diag->LastDumpType), diag->LastDumpType);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "IRQL/CPU  : %lu / %lu", diag->Irql, diag->Cpu);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "DUMP I/O  : offset=0x%p length=0x%lX", (PVOID)(ULONG_PTR)diag->DumpOffset, diag->DumpBufferLength);
    y += 6;
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "MODULE    : %s", diag->CandidateModule);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "ADDRESS   : 0x%p", (PVOID)diag->CandidateAddress);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "MOD RANGE : base=0x%p size=0x%lX off=0x%p", (PVOID)diag->CandidateModuleBase, diag->CandidateModuleSize, (PVOID)diag->CandidateModuleOffset);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "CAND SRC  : param%lu / %s", diag->CandidateParameter, diag->CandidateSource);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "CLASS     : %s", KswordARKBugcheckModuleClassText(diag->CandidateClass));
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "CONFIDENCE: %s", KswordARKBugcheckConfidenceText(diag->CandidateConfidence));
    y += 6;
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "DRIVER    : DriverObject=0x%p DeviceObject=0x%p", State->DriverObject, State->DeviceObject);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "CALLBACKS : BC=%u TRIAGE=%u DUMP=%u SECONDARY=%u", State->ClassicRegistered ? 1UL : 0UL, State->TriageRegistered ? 1UL : 0UL, State->DumpIoRegistered ? 1UL : 0UL, State->SecondaryRegistered ? 1UL : 0UL);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "MODULES   : cached=%lu", State->ModuleCount);
    y += 6;
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "SVGA PCI  : %lu:%lu.%lu vendor=0x%04X device=0x%04X", svga->Bus, svga->Device, svga->Function, svga->VendorId, svga->DeviceId);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "SVGA MODE : %lux%lux%lu pitch=%lu depth=%lu", svga->Width, svga->Height, svga->Bpp, svga->Pitch, svga->Depth);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "SVGA FB   : phys=0x%p va=0x%p bytes=0x%p", (PVOID)(ULONG_PTR)svga->FramebufferPhysical.QuadPart, (PVOID)svga->Framebuffer, (PVOID)(ULONG_PTR)svga->FramebufferLength);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "SVGA FIFO : phys=0x%p va=0x%p bytes=0x%p", (PVOID)(ULONG_PTR)svga->FifoPhysical.QuadPart, (PVOID)svga->Fifo, (PVOID)(ULONG_PTR)svga->FifoLength);
    KswordARKSvgaDrawPanelLine(svga, 28, &y, brand, "BITMAP    : uploaded=%u %lux%lu bytes=0x%lX color=#%06lX", InterlockedCompareExchange(&State->Bitmap.Valid, 1, 1) != 0 ? 1UL : 0UL, State->Bitmap.Width, State->Bitmap.Height, State->Bitmap.DataLength, State->Bitmap.BrandColorRgb & 0xFFFFFFUL);

    KeMemoryBarrier();
    if (!KswordARKSvgaFifoUpdateNoLog(svga, 0, 0, svga->Width, svga->Height)) {
        KswordARKSvgaPortSyncNoLog(svga);
    }
}

#else

NTSTATUS
KswordARKBugcheckSvgaInitialize(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    if (Context != NULL) {
        RtlZeroMemory(Context, sizeof(*Context));
    }
    return STATUS_NOT_SUPPORTED;
}

VOID
KswordARKBugcheckSvgaShutdown(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    if (Context != NULL) {
        RtlZeroMemory(Context, sizeof(*Context));
    }
}

VOID
KswordARKBugcheckSvgaModeSetNoLog(
    _Inout_ PKSWORD_ARK_SVGA_CONTEXT Context
    )
{
    UNREFERENCED_PARAMETER(Context);
}

VOID
KswordARKBugcheckSvgaDrawPanelNoLog(
    _Inout_ PKSWORD_ARK_BUGCHECK_STATE State
    )
{
    UNREFERENCED_PARAMETER(State);
}

#endif
