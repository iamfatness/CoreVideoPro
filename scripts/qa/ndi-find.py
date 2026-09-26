"""Discover NDI sources through the installed SDK runtime (no UI automation)."""

import argparse
import ctypes
import json
import time


class FindCreate(ctypes.Structure):
    _fields_ = [
        ("show_local_sources", ctypes.c_bool),
        ("p_groups", ctypes.c_char_p),
        ("p_extra_ips", ctypes.c_char_p),
    ]


class Source(ctypes.Structure):
    _fields_ = [("p_ndi_name", ctypes.c_char_p), ("p_url_address", ctypes.c_char_p)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--seconds", type=float, default=8)
    args = parser.parse_args()

    ndi = ctypes.CDLL(args.runtime)
    ndi.NDIlib_initialize.argtypes = []
    ndi.NDIlib_initialize.restype = ctypes.c_bool
    ndi.NDIlib_find_create_v2.argtypes = [ctypes.POINTER(FindCreate)]
    ndi.NDIlib_find_create_v2.restype = ctypes.c_void_p
    ndi.NDIlib_find_wait_for_sources.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    ndi.NDIlib_find_wait_for_sources.restype = ctypes.c_bool
    ndi.NDIlib_find_get_current_sources.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    ndi.NDIlib_find_get_current_sources.restype = ctypes.POINTER(Source)
    ndi.NDIlib_find_destroy.argtypes = [ctypes.c_void_p]

    if not ndi.NDIlib_initialize():
        raise RuntimeError("NDIlib_initialize failed")
    finder = ndi.NDIlib_find_create_v2(ctypes.byref(FindCreate(True, None, None)))
    if not finder:
        raise RuntimeError("NDIlib_find_create_v2 failed")
    try:
        names = {}
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            ndi.NDIlib_find_wait_for_sources(finder, 500)
            count = ctypes.c_uint32()
            sources = ndi.NDIlib_find_get_current_sources(finder, ctypes.byref(count))
            for index in range(count.value):
                source = sources[index]
                name = (source.p_ndi_name or b"").decode("utf-8", "replace")
                url = (source.p_url_address or b"").decode("utf-8", "replace")
                if name:
                    names[name] = url
        print(json.dumps(names, sort_keys=True))
    finally:
        ndi.NDIlib_find_destroy(finder)


if __name__ == "__main__":
    main()
