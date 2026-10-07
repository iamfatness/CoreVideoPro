"""Owned, non-activating motion window for native WGC capture diagnostics."""
import ctypes
from ctypes import wintypes
import threading


class WgcTestMotion:
    def __init__(self, monitor_index):
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self.run, args=(monitor_index,))
        self.thread.start()
        if not self.ready.wait(10) or self.error:
            self.close()
            raise RuntimeError(self.error or "WGC motion window startup timed out")

    def run(self, monitor_index):
        window = None
        try:
            user = ctypes.WinDLL("user32", use_last_error=True)
            gdi = ctypes.WinDLL("gdi32", use_last_error=True)
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC,
                                               ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
            origins = []

            @callback_type
            def monitor_callback(monitor, dc, rect, data):
                origins.append(((rect.contents.left + rect.contents.right) // 2 - 80,
                                (rect.contents.top + rect.contents.bottom) // 2 - 45))
                return True

            user.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.POINTER(wintypes.RECT), callback_type, wintypes.LPARAM]
            user.EnumDisplayMonitors(None, None, monitor_callback, 0)
            if not 0 <= monitor_index < len(origins):
                raise RuntimeError("selected WGC monitor is unavailable")
            user.CreateWindowExW.restype = wintypes.HWND
            user.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR,
                wintypes.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
            kernel.GetModuleHandleW.restype = wintypes.HMODULE
            kernel.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
            x, y = origins[monitor_index]
            window = user.CreateWindowExW(0x08000000 | 0x80 | 0x8, "STATIC", "CoreVideo WGC capture validation",
                0x80000000, x, y, 160, 90, None, None, kernel.GetModuleHandleW(None), None)
            if not window:
                raise ctypes.WinError(ctypes.get_last_error())
            user.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
            user.ShowWindow(window, 4)  # SW_SHOWNOACTIVATE
            user.GetDC.restype = wintypes.HDC
            user.GetDC.argtypes = [wintypes.HWND]
            user.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
            user.FillRect.argtypes = [wintypes.HDC, ctypes.POINTER(wintypes.RECT), wintypes.HBRUSH]
            gdi.CreateSolidBrush.restype = wintypes.HBRUSH
            gdi.CreateSolidBrush.argtypes = [wintypes.DWORD]
            gdi.DeleteObject.argtypes = [wintypes.HGDIOBJ]
            user.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT, wintypes.UINT]
            user.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
            user.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]
            user.DestroyWindow.argtypes = [wintypes.HWND]
            self.ready.set()
            sequence = 0
            while not self.stop_event.is_set():
                message = wintypes.MSG()
                while user.PeekMessageW(ctypes.byref(message), None, 0, 0, 1):
                    user.TranslateMessage(ctypes.byref(message))
                    user.DispatchMessageW(ctypes.byref(message))
                dc = user.GetDC(window)
                brush = gdi.CreateSolidBrush((sequence % 256) | (64 << 8) | (192 << 16))
                try:
                    user.FillRect(dc, ctypes.byref(wintypes.RECT(0, 0, 160, 90)), brush)
                    gdi.GdiFlush()
                finally:
                    gdi.DeleteObject(brush)
                    user.ReleaseDC(window, dc)
                sequence += 1
                self.stop_event.wait(1 / 60)
        except Exception as error:
            self.error = str(error)
            self.ready.set()
        finally:
            if window:
                user.DestroyWindow(window)

    def close(self):
        self.stop_event.set()
        self.thread.join()
