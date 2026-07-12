# EASY-HWID-SPOOFER Notice

- Upstream: https://github.com/FiYHer/EASY-HWID-SPOOFER
- License: GNU General Public License v3.0, copied in `LICENSE.txt`.
- KswordARK integration scope: only the dispatch-function approach is represented in KswordARK (`IRP_MJ_DEVICE_CONTROL` hook plus completion-routine return-buffer rewriting).
- Excluded scope: physical-memory HWID modification, SMBIOS physical table scanning/writing, storport private memory serial rewriting, NDIS private block scanning/writing, and boot-sector/volume direct writes.
- Local integration points: `shared/driver/KswordArkHwidIoctl.h`, `KswordARKDriver/src/features/hwid/`, `Ksword5.1/Ksword5.1/ArkDriverClient/ArkDriverHwid.cpp`, and `Ksword5.1/Ksword5.1/HardwareDock/HardwareHwidDispatchPage.*`.
