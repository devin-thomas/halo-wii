#!/usr/bin/env python3
"""Generate port/wii/engine/unsupported_sdk.c (HWI-015).

The Wii engine build implements the Xbox SDK services the engine needs on
the Wii (port/wii/engine/wii_*.c, and the reused Linux files) and nothing
else. Every other SDK entry point the engine links against is defined here,
with the SDK's own prototype (port/include/xdk/xdk_pdb.h, xdk_xbdm.h), so the
compiler checks each definition against the declaration the engine calls.
Each one reports itself (wii_unsupported) and fails the call with the
failure value its interface defines: E_FAIL for HRESULTs, NULL for
pointers and handles, SOCKET_ERROR/INVALID_SOCKET for Winsock, an error
code for the XInput/XNet status returns, FALSE/0 otherwise. Nothing here
reports success.

`python tools/wii/engine_unsupported.py --check` fails when the committed
file differs from what this script generates (line endings ignored).
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "port/wii/engine/unsupported_sdk.c"
HEADERS = [ROOT / "port/include/xdk/xdk_pdb.h", ROOT / "port/include/xdk/xdk_xbdm.h"]

# subsystem -> SDK names (in xdk_pdb.h / xdk_xbdm.h); halo_ws_<name> is
# Winsock's <name> under the port's private spelling
# (port/linux/include/halo_linux_winsock_names.h)
SUBSYSTEMS: Dict[str, List[str]] = {
    "d3d8": [
        "D3DCubeTexture_LockRect", "D3DDevice_Begin", "D3DDevice_BeginVisibilityTest",
        "D3DDevice_BlockUntilVerticalBlank", "D3DDevice_Clear", "D3DDevice_CreateCubeTexture",
        "D3DDevice_CreateIndexBuffer", "D3DDevice_CreatePalette", "D3DDevice_CreateTexture",
        "D3DDevice_CreateVertexBuffer", "D3DDevice_CreateVertexShader", "D3DDevice_CreateVolumeTexture",
        "D3DDevice_DeleteVertexShader", "D3DDevice_DrawIndexedVertices", "D3DDevice_DrawVertices",
        "D3DDevice_End", "D3DDevice_EndVisibilityTest", "D3DDevice_GetBackBuffer",
        "D3DDevice_GetDepthStencilSurface", "D3DDevice_GetDeviceCaps", "D3DDevice_GetTransform",
        "D3DDevice_GetVertexShaderSize", "D3DDevice_GetVisibilityTestResult", "D3DDevice_InsertCallback",
        "D3DDevice_IsBusy", "D3DDevice_KickPushBuffer", "D3DDevice_LoadVertexShader",
        "D3DDevice_PersistDisplay", "D3DDevice_Present", "D3DDevice_Release", "D3DDevice_SelectVertexShader",
        "D3DDevice_SetFlickerFilter", "D3DDevice_SetIndices", "D3DDevice_SetPalette",
        "D3DDevice_SetPixelShaderProgram", "D3DDevice_SetRenderStateNotInline",
        "D3DDevice_SetRenderState_BackFillMode", "D3DDevice_SetRenderState_CullMode",
        "D3DDevice_SetRenderState_Deferred", "D3DDevice_SetRenderState_DoNotCullUncompressed",
        "D3DDevice_SetRenderState_Dxt1NoiseEnable", "D3DDevice_SetRenderState_EdgeAntiAlias",
        "D3DDevice_SetRenderState_FillMode", "D3DDevice_SetRenderState_FogColor",
        "D3DDevice_SetRenderState_FrontFace", "D3DDevice_SetRenderState_LineWidth",
        "D3DDevice_SetRenderState_LogicOp", "D3DDevice_SetRenderState_MultiSampleAntiAlias",
        "D3DDevice_SetRenderState_MultiSampleMask", "D3DDevice_SetRenderState_MultiSampleType",
        "D3DDevice_SetRenderState_NormalizeNormals", "D3DDevice_SetRenderState_OcclusionCullEnable",
        "D3DDevice_SetRenderState_PSTextureModes", "D3DDevice_SetRenderState_RopZCmpAlwaysRead",
        "D3DDevice_SetRenderState_RopZRead", "D3DDevice_SetRenderState_ShadowFunc",
        "D3DDevice_SetRenderState_Simple", "D3DDevice_SetRenderState_StencilCullEnable",
        "D3DDevice_SetRenderState_StencilEnable", "D3DDevice_SetRenderState_StencilFail",
        "D3DDevice_SetRenderState_TextureFactor", "D3DDevice_SetRenderState_TwoSidedLighting",
        "D3DDevice_SetRenderState_VertexBlend", "D3DDevice_SetRenderState_YuvEnable",
        "D3DDevice_SetRenderState_ZBias", "D3DDevice_SetRenderState_ZEnable", "D3DDevice_SetRenderTarget",
        "D3DDevice_SetShaderConstantMode", "D3DDevice_SetSoftDisplayFilter", "D3DDevice_SetStreamSource",
        "D3DDevice_SetTexture", "D3DDevice_SetTextureState_BorderColor", "D3DDevice_SetTextureState_BumpEnv",
        "D3DDevice_SetTextureState_ColorKeyColor", "D3DDevice_SetTextureState_Deferred",
        "D3DDevice_SetTextureState_TexCoordIndex", "D3DDevice_SetTransform", "D3DDevice_SetVertexData2f",
        "D3DDevice_SetVertexData2s", "D3DDevice_SetVertexData4f", "D3DDevice_SetVertexData4ub",
        "D3DDevice_SetVertexDataColor", "D3DDevice_SetVertexShader", "D3DDevice_SetVertexShaderConstant",
        "D3DDevice_SetVerticalBlankCallback", "D3DDevice_SetViewport", "D3DPalette_Lock",
        "D3DResource_BlockUntilNotBusy", "D3DResource_IsBusy", "D3DResource_Register", "D3DResource_Release",
        "D3DSurface_GetDesc", "D3DSurface_LockRect", "D3DTexture_GetLevelDesc", "D3DTexture_GetSurfaceLevel",
        "D3DTexture_LockRect", "D3DVertexBuffer_Lock", "D3DVolumeTexture_LockBox", "D3DXMatrixOrthoLH",
        "D3DXMatrixPerspectiveLH", "D3DXVec4Transform", "Direct3DCreate8", "Direct3D_CreateDevice",
        "Direct3D_SetPushBufferSize",
    ],
    "dsound": [
        "DirectSoundCreate", "DirectSoundCreateBuffer", "DirectSoundDoWork", "DirectSoundUseFullHRTF",
        "IDirectSoundBuffer_Play", "IDirectSoundBuffer_Release", "IDirectSoundBuffer_SetBufferData",
        "IDirectSoundBuffer_SetCurrentPosition", "IDirectSoundBuffer_SetLoopRegion",
        "IDirectSoundBuffer_SetPitch", "IDirectSoundBuffer_SetVolume", "IDirectSoundBuffer_Stop",
        "IDirectSoundStream_SetConeAngles", "IDirectSoundStream_SetConeOrientation",
        "IDirectSoundStream_SetConeOutsideVolume", "IDirectSoundStream_SetFrequency",
        "IDirectSoundStream_SetI3DL2Source", "IDirectSoundStream_SetMaxDistance",
        "IDirectSoundStream_SetMinDistance", "IDirectSoundStream_SetMixBinVolumes",
        "IDirectSoundStream_SetMixBins", "IDirectSoundStream_SetMode", "IDirectSoundStream_SetPosition",
        "IDirectSoundStream_SetVelocity", "IDirectSoundStream_SetVolume", "IDirectSound_CommitDeferredSettings",
        "IDirectSound_CreateSoundBuffer", "IDirectSound_CreateSoundStream", "IDirectSound_DownloadEffectsImage",
        "IDirectSound_GetCaps", "IDirectSound_GetSpeakerConfig", "IDirectSound_Release",
        "IDirectSound_SetDistanceFactor", "IDirectSound_SetI3DL2Listener", "IDirectSound_SetMixBinHeadroom",
        "IDirectSound_SetOrientation", "IDirectSound_SetPosition", "IDirectSound_SetRolloffFactor",
        "IDirectSound_SetVelocity",
    ],
    "input": [
        "XGetDeviceChanges", "XInitDevices", "XInputClose", "XInputDebugGetKeystroke",
        "XInputDebugInitKeyboardQueue", "XInputGetState", "XInputOpen", "XInputSetState",
    ],
    "network": [
        "WSACleanup", "WSAGetLastError", "WSAStartup", "XNetCleanup", "XNetCreateKey",
        "XNetGetEthernetLinkStatus", "XNetGetTitleXnAddr", "XNetRandom", "XNetRegisterKey", "XNetStartup",
        "XNetUnregisterKey", "XNetXnAddrToInAddr", "__WSAFDIsSet",
        *(f"halo_ws_{name}" for name in (
            "accept", "bind", "closesocket", "connect", "getpeername", "getsockname", "getsockopt",
            "ioctlsocket", "listen", "recv", "recvfrom", "select", "send", "sendto", "setsockopt", "socket")),
    ],
    "xbdm": ["DmCloseModuleSections", "DmWalkLoadedModules", "DmWalkModuleSections"],
}

# failure values where the type's default (0, NULL, E_FAIL) would read as success
FAILURE: Dict[str, str] = {
    "WSAStartup": "WSASYSNOTREADY",
    "WSACleanup": "SOCKET_ERROR",
    "WSAGetLastError": "WSASYSNOTREADY",
    "XNetStartup": "WSASYSNOTREADY",
    "XNetCleanup": "WSASYSNOTREADY",
    "XNetCreateKey": "WSASYSNOTREADY",
    "XNetRegisterKey": "WSASYSNOTREADY",
    "XNetUnregisterKey": "WSASYSNOTREADY",
    "XNetXnAddrToInAddr": "WSASYSNOTREADY",
    "XNetRandom": "WSASYSNOTREADY",
    "XNetGetTitleXnAddr": "XNET_GET_XNADDR_NONE",
    "XInputGetState": "ERROR_DEVICE_NOT_CONNECTED",
    "XInputSetState": "ERROR_DEVICE_NOT_CONNECTED",
    "XInputDebugGetKeystroke": "ERROR_DEVICE_NOT_CONNECTED",
    "XInputDebugInitKeyboardQueue": "ERROR_DEVICE_NOT_CONNECTED",
    "halo_ws_socket": "INVALID_SOCKET",
    "halo_ws_accept": "INVALID_SOCKET",
    "DmWalkLoadedModules": "E_FAIL",
    "DmWalkModuleSections": "E_FAIL",
    "DmCloseModuleSections": "E_FAIL",
    "D3DResource_IsBusy": "FALSE",
    "D3DDevice_IsBusy": "FALSE",
}
SOCKET_ERROR_NAMES = {f"halo_ws_{name}" for name in (
    "bind", "closesocket", "connect", "getpeername", "getsockname", "getsockopt", "ioctlsocket", "listen",
    "recv", "recvfrom", "select", "send", "sendto", "setsockopt")}

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def prototypes() -> Dict[str, Tuple[str, str]]:
    """name -> (return type and calling convention, parameter list)"""
    found: Dict[str, Tuple[str, str]] = {}
    pattern = re.compile(r"^([^;(){}#]*?)\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*;", re.M)
    for header in HEADERS:
        text = COMMENT.sub(" ", header.read_text(encoding="latin-1"))
        for match in pattern.finditer(text):
            result, name, parameters = match.group(1).strip(), match.group(2), match.group(3)
            if result and not result.startswith(("typedef", "return", "extern \"")):
                found.setdefault(name, (" ".join(result.split()), " ".join(parameters.split())))
    return found


def split_parameters(parameters: str) -> List[str]:
    items, depth, current = [], 0, ""
    for character in parameters:
        if character == "," and depth == 0:
            items.append(current.strip())
            current = ""
            continue
        depth += character in "(["
        depth -= character in ")]"
        current += character
    if current.strip():
        items.append(current.strip())
    return items


def named(parameter: str, index: int) -> str:
    """the parameter with a name: inside a function pointer declarator,
    before an array bound, or at the end"""
    name = f"a{index}"
    pointer = re.search(r"\(\s*(?:__\w+\s+)?\*", parameter)
    if pointer:
        return parameter[:pointer.end()] + name + parameter[pointer.end():]
    bracket = parameter.find("[")
    if bracket >= 0:
        return parameter[:bracket].rstrip() + " " + name + parameter[bracket:]
    if re.search(r"\b(?:a\d+|[a-z_]+)$", parameter) and not re.search(
            r"\b(?:int|long|short|char|float|double|void|unsigned|signed|DWORD|BOOL|HANDLE|LPVOID)$", parameter) \
            and " " in parameter and not parameter.endswith("*"):
        # already named (xdk_xbdm.h names its parameters)
        return parameter
    return f"{parameter} {name}"


def failure(name: str, result: str) -> Optional[str]:
    """the failure value returned, or None for void"""
    base = re.sub(r"\b__(?:stdcall|fastcall|cdecl)\b", "", result).strip()
    if name in FAILURE:
        return FAILURE[name]
    if name in SOCKET_ERROR_NAMES:
        return "SOCKET_ERROR"
    if base == "void":
        return None
    if base.endswith("*"):
        return "NULL"
    if base in ("long", "HRESULT") and (name.startswith(("D3D", "Direct", "IDirectSound", "DirectSound"))):
        return "E_FAIL"
    if base in ("float", "double"):
        return "0.0f"
    return "0"


def subsystem_of(name: str) -> str:
    for subsystem, names in SUBSYSTEMS.items():
        if name in names:
            return subsystem
    raise KeyError(name)


def generate() -> str:
    found = prototypes()
    lines = [
        "/* generated by tools/wii/engine_unsupported.py - do not edit",
        "",
        "Xbox SDK entry points the Wii engine build does not implement (HWI-015).",
        "Each has the SDK's prototype, reports itself as unsupported and fails",
        "(see the script). */",
        "",
        '#include "platform.h"',
        '#include "wii_platform.h"',
    ]
    for subsystem, names in SUBSYSTEMS.items():
        lines += ["", f"/* ---------- {subsystem} */"]
        for name in sorted(names):
            source = name[len("halo_ws_"):] if name.startswith("halo_ws_") else name
            if source not in found:
                raise SystemExit(f"no SDK prototype for {source}")
            result, parameters = found[source]
            # the XDK's Winsock types under the port's private names, as
            # platform.h reads the SDK headers (halo_linux_winsock_names.h)
            parameters = re.sub(r"\bstruct (fd_set|timeval)\b", r"struct halo_ws_\1", parameters)
            items = split_parameters(parameters)
            if items in ([], ["void"]):
                declared = "void"
            else:
                declared = ", ".join(item if item == "..." else named(item, index)
                                     for index, item in enumerate(items))
            value = failure(name, result)
            lines += ["", f"{result} {name}({declared})", "{",
                      f'\twii_unsupported("{subsystem}", "{name}");']
            if value is not None:
                lines.append(f"\treturn {value};")
            lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the committed file is out of date")
    args = parser.parse_args()
    text = generate()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8").replace("\r\n", "\n") if OUTPUT.exists() else ""
        if current != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date: run tools/wii/engine_unsupported.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
