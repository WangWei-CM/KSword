# Ksword System Informer DynData Integration

This directory vendors the System Informer generated dynamic-offset data used by
KswordARKDriver to match kernel profiles by module class, PE machine,
TimeDateStamp, and SizeOfImage.

Files:

- `kphdyn.xml`: original auditable source data.
- `kphdyn.c`: generated embedded byte array. The include was changed from
  `kphlibbase.h` to `ksw_si_dynconfig.h` so it can compile without KPH.
- `kphdyn.h`: generated structure definitions. The include was changed from
  `kphlibbase.h` to `ntddk.h` for standalone kernel-driver compilation.
- `ksw_si_dynconfig.h`: Ksword-only wrapper for DDK types and `ARRAYSIZE`.
- `LICENSE.txt` and `NOTICE.md`: required attribution material.

Do not mix Ksword driver IOCTL protocol definitions into this directory. Shared
R0/R3 protocol remains under `shared/driver/`.
