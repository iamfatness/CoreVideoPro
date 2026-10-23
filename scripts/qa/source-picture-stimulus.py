"""Owned non-activating screen stimulus with independently logged picture identities.

Run in the interactive desktop, on the captured display. GDI double buffering
keeps the two complementary guard bands in the same submitted picture. This is
an acquisition stimulus, never evidence of CoreVideo delivery by itself.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import time
from pathlib import Path


def run(output, x, y, seconds):
    output = Path(output)
    if output.exists():
        raise ValueError("Preserve existing stimulus evidence")
    user, gdi, kernel = (c.WinDLL(name, use_last_error=True) for name in ("user32", "gdi32", "kernel32"))
    user.CreateWindowExW.argtypes = [w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD, c.c_int, c.c_int, c.c_int, c.c_int, w.HWND, w.HMENU, w.HINSTANCE, w.LPVOID]
    user.CreateWindowExW.restype = w.HWND
    kernel.GetModuleHandleW.argtypes = [w.LPCWSTR]
    kernel.GetModuleHandleW.restype = w.HMODULE
    user.GetDC.argtypes, user.GetDC.restype = [w.HWND], w.HDC
    user.ReleaseDC.argtypes = [w.HWND, w.HDC]
    user.ShowWindow.argtypes = [w.HWND, c.c_int]
    user.DestroyWindow.argtypes = [w.HWND]
    user.FillRect.argtypes = [w.HDC, c.POINTER(w.RECT), w.HBRUSH]
    user.PeekMessageW.argtypes = [c.POINTER(w.MSG), w.HWND, w.UINT, w.UINT, w.UINT]
    user.TranslateMessage.argtypes = [c.POINTER(w.MSG)]
    user.DispatchMessageW.argtypes = [c.POINTER(w.MSG)]
    gdi.CreateCompatibleDC.argtypes, gdi.CreateCompatibleDC.restype = [w.HDC], w.HDC
    gdi.CreateCompatibleBitmap.argtypes, gdi.CreateCompatibleBitmap.restype = [w.HDC, c.c_int, c.c_int], w.HBITMAP
    gdi.SelectObject.argtypes, gdi.SelectObject.restype = [w.HDC, w.HGDIOBJ], w.HGDIOBJ
    gdi.CreateSolidBrush.argtypes, gdi.CreateSolidBrush.restype = [w.DWORD], w.HBRUSH
    gdi.DeleteObject.argtypes = [w.HGDIOBJ]
    gdi.DeleteDC.argtypes = [w.HDC]
    gdi.BitBlt.argtypes = [w.HDC, c.c_int, c.c_int, c.c_int, c.c_int, w.HDC, c.c_int, c.c_int, w.DWORD]
    window = user.CreateWindowExW(0x08000000 | 0x80 | 0x8, "STATIC", "CoreVideo source picture test", 0x80000000,
                                 x, y, 720, 128, None, None, kernel.GetModuleHandleW(None), None)
    if not window:
        raise c.WinError(c.get_last_error())
    dc = user.GetDC(window)
    memory = gdi.CreateCompatibleDC(dc)
    bitmap = gdi.CreateCompatibleBitmap(dc, 720, 128)
    old = gdi.SelectObject(memory, bitmap)
    brushes = [gdi.CreateSolidBrush(color) for color in (0x000000, 0xffffff, 0x4080c0)]
    if not all((dc, memory, bitmap, old, *brushes)):
        raise RuntimeError("Screen stimulus resource creation failed")
    user.ShowWindow(window, 4)
    try:
        with output.open("x", encoding="utf8") as log:
            log.write(json.dumps({"schema": "source-picture-stimulus-v1", "x": x, "y": y,
                                  "width": 720, "height": 128, "nominalHz": 10,
                                  "clock": "Windows QPC converted to nanoseconds"}) + "\n")
            started, sequence = time.perf_counter(), 0
            while time.perf_counter() - started < seconds:
                message = w.MSG()
                while user.PeekMessageW(c.byref(message), None, 0, 0, 1):
                    user.TranslateMessage(c.byref(message)); user.DispatchMessageW(c.byref(message))
                sequence += 1
                user.FillRect(memory, c.byref(w.RECT(0, 0, 720, 128)), brushes[2])
                for cell in range(36):
                    bit = cell in (0, 35) if cell in (0, 1, 34, 35) else bool(sequence & (1 << (cell - 2)))
                    user.FillRect(memory, c.byref(w.RECT(cell * 20, 0, (cell + 1) * 20, 32)), brushes[int(bit)])
                    user.FillRect(memory, c.byref(w.RECT(cell * 20, 96, (cell + 1) * 20, 128)), brushes[int(not bit)])
                before = time.perf_counter_ns()
                if not gdi.BitBlt(dc, 0, 0, 720, 128, memory, 0, 0, 0x00CC0020):
                    raise c.WinError(c.get_last_error())
                gdi.GdiFlush()
                log.write(json.dumps({"identity": sequence, "beforeNs": before, "afterNs": time.perf_counter_ns()}) + "\n")
                log.flush()
                time.sleep(max(0, started + sequence / 10 - time.perf_counter()))
            log.write(json.dumps({"complete": True, "pictures": sequence}) + "\n")
    finally:
        for brush in brushes: gdi.DeleteObject(brush)
        gdi.SelectObject(memory, old); gdi.DeleteObject(bitmap); gdi.DeleteDC(memory)
        user.ReleaseDC(window, dc); user.DestroyWindow(window)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output")
    parser.add_argument("--x", type=int, required=True)
    parser.add_argument("--y", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=2100)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 7500: parser.error("seconds must be 1..7500")
    run(args.output, args.x, args.y, args.seconds)
