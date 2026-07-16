# Third-Party Notices

The Ksword project-owned material is licensed under GPL-3.0-only as stated in
`PROJECT_LICENSE.md`. Third-party components remain under their own terms. This
file is an inventory and notice; it does not replace the referenced license
texts or grant additional rights.

## Components stored in this repository

| Component | Used or stored at | License material |
| --- | --- | --- |
| System Informer DynData | `third_party/systeminformer_dyn/` | MIT; `third_party/systeminformer_dyn/LICENSE.txt` and `NOTICE.md` |
| EASY-HWID-SPOOFER provenance | `third_party/easy_hwid_spoofer/`; related code under `KswordARKDriver/src/features/hwid/` | GPL-3.0; `third_party/easy_hwid_spoofer/LICENSE.txt` and `NOTICE.md`; license compatibility is resolved by the project's GPL-3.0-only choice, while exact provenance/source packaging remains required |
| FLTK 1.4.4 | `KswordSetup/fltk/` | GNU Library GPL v2 with FLTK exceptions; `third_party/fltk/LICENSE.txt` and `NOTICE.md` |
| Qt Advanced Docking System | `Ksword5.1/Ksword5.1/include/ads/` and `lib/qtadvanceddocking*` | LGPL-2.1-or-later; `third_party/qt_advanced_docking_system/LICENSE.txt` and `NOTICE.md` |

## Qt 6.9.3 runtime

Release archives include Qt DLLs. The currently used Qt Charts module is
available under a Qt commercial license or GPL-3.0-only, not LGPL. Ksword has
selected the GPL-3.0-only route for the project-owned parts of the main program
and HUD that combine with Qt Charts. This closes the former root-license
incompatibility; it does not waive any source, notice, or build-material duty.

Other dynamically linked Qt modules must be mapped to their applicable
open-source GPL/LGPL terms for the release. When an LGPL option is selected, the
release must include the required license notices, corresponding Qt library
source or compliant written source offer, and instructions/conditions that
preserve the recipient's ability to replace and relink the libraries.

The matching Qt GPL/LGPL texts, copyright notices, SBOM/module inventory, and
Corresponding Source instructions must be added to each release package.

## Release status

`docs/许可证兼容性审计.md` records the current compatibility analysis and
remaining release-material blockers. A root project license, community
covenant, or this notice cannot override GPL, LGPL, commercial-license, or
other third-party requirements.
