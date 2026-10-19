# Third-party notices

## OpenVR SDK 2.15.6

`third_party/openvr` holds `openvr.h`, `openvr_driver.h`, `openvr_api.lib` and
`openvr_api.dll` from ValveSoftware/openvr tag `v2.15.6`. The release ships
`openvr_api.dll` next to `QuestLHSync.exe`.

Copyright (c) 2015, Valve Corporation. Licensed under the 3-clause BSD license;
the complete text is in `third_party/openvr/LICENSE`.

Source: https://github.com/ValveSoftware/openvr

## MinHook

MinHook under `third_party/minhook` is Copyright (C) 2009-2017 Tsuda Kageyu,
with Hacker Disassembler Engine portions Copyright (c) 2008-2009 Vyacheslav
Patkov. It is distributed under a 2-clause BSD license; see
`third_party/minhook/LICENSE.txt`.

Source: https://github.com/TsudaKageyu/minhook

The Linux lighthouse shim uses MinHook's Hacker Disassembler Engine (`hde64`)
alone, for its own small hook (`src/linux/shim/inline_hook.cpp`); `pstdint.h`
there gains a non-Windows branch.

## Monado headers

`third_party/monado` holds libmonado's interface (`monado/monado.h`) and the
`XR_MNDX_xdev_space` extension's header (`openxr/XR_MNDX_xdev_space.h`,
`openxr/openxr_extension_helpers.h`) from Monado, for the Linux build.

Copyright 2019-2026, Collabora, Ltd. and NVIDIA CORPORATION. Licensed under the
Boost Software License 1.0; see `third_party/monado/LICENSE`.

Source: https://gitlab.freedesktop.org/monado/monado

## Frida 17.10.0

The Magisk module ships `frida-inject` 17.10.0 for android-arm64, unmodified,
as published on Frida's GitHub release. It is not in this repository:
`magisk/build_module.py` downloads it and checks its SHA-256.

Copyright (C) Ole André Vadla Ravnås and contributors. Licensed under the
wxWindows Library Licence, Version 3.1.

Source: https://github.com/frida/frida
