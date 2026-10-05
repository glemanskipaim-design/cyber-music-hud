// P/Invoke compartilhado pelos scripts de teste (tests\e2e.ps1, tools\snapshot.ps1).
using System;
using System.Drawing;
using System.Runtime.InteropServices;

public static class Native {
    // Coordenadas reais de pixel (o HUD é PerMonitorV2); sem isso a captura sai deslocada.
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    static Native() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }  // PER_MONITOR_AWARE_V2

    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public static uint ForegroundPid() { uint pid; GetWindowThreadProcessId(GetForegroundWindow(), out pid); return pid; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int index);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h, IntPtr src, int sx, int sy, uint rop);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public const long WS_EX_TOPMOST = 0x8, WS_EX_TRANSPARENT = 0x20, WS_EX_TOOLWINDOW = 0x80,
                      WS_EX_LAYERED = 0x80000, WS_EX_NOACTIVATE = 0x8000000;

    public static long ExStyle(IntPtr h) { return GetWindowLongPtr(h, -20).ToInt64(); }

    public static RECT Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }

    // CAPTUREBLT inclui janelas em camadas (o HUD) na captura.
    public static void Capture(IntPtr h, string path) {
        RECT r = Rect(h);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        int pad = 24;
        using (var bmp = new Bitmap(w + 2 * pad, ht + 2 * pad))
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr dst = g.GetHdc();
            IntPtr src = GetDC(IntPtr.Zero);
            BitBlt(dst, 0, 0, w + 2 * pad, ht + 2 * pad, src, r.Left - pad, r.Top - pad, 0x00CC0020 | 0x40000000);
            ReleaseDC(IntPtr.Zero, src);
            g.ReleaseHdc(dst);
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
