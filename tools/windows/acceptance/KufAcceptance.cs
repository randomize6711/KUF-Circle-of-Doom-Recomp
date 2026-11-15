// Win32 helpers of the Windows acceptance test (Run-Acceptance.ps1; docs/WINDOWS_TEST.md). Loaded with Add-Type
// (C# 5, .NET Framework 4: Windows PowerShell 5.1 as Windows has it). Nothing here changes system settings: it
// starts the test's own programs, finds their windows, presses keys and moves the mouse like a person would (Alt+Tab,
// a drag from a helper window), takes screenshots and reads controllers (XInput).
using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.Specialized;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

public static class KufAcc {
  // ---- processes -----------------------------------------------------------------------------------------------
  // One argument as CommandLineToArgvW reads it back (quotes, backslashes before quotes doubled).
  public static string Quote(string a) {
    if (a.Length > 0 && a.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return a;
    var sb = new StringBuilder("\"");
    int slashes = 0;
    foreach (char c in a) {
      if (c == '\\') { slashes++; continue; }
      if (c == '"') { sb.Append('\\', slashes * 2 + 1); sb.Append('"'); slashes = 0; continue; }
      sb.Append('\\', slashes); slashes = 0; sb.Append(c);
    }
    sb.Append('\\', slashes * 2);
    return sb.Append('"').ToString();
  }

  public static string Join(string[] args) {
    var parts = new List<string>();
    foreach (string a in args) parts.Add(Quote(a));
    return string.Join(" ", parts.ToArray());
  }

  // Starts exe with args (each quoted), env added to this process's, stdout and stderr appended to outPath.
  public static Process Start(string exe, string[] args, string cwd, Hashtable env, string outPath) {
    var psi = new ProcessStartInfo(exe, Join(args));
    psi.UseShellExecute = false;
    psi.WorkingDirectory = cwd;
    psi.RedirectStandardOutput = true;
    psi.RedirectStandardError = true;
    psi.StandardOutputEncoding = Encoding.UTF8;
    psi.StandardErrorEncoding = Encoding.UTF8;
    psi.CreateNoWindow = true;
    if (env != null) foreach (DictionaryEntry e in env) psi.EnvironmentVariables[(string)e.Key] = (string)e.Value;
    var writer = new StreamWriter(outPath, true, new UTF8Encoding(false));
    writer.AutoFlush = true;
    var gate = new object();
    var p = new Process();
    p.StartInfo = psi;
    DataReceivedEventHandler h = (s, ev) => { if (ev.Data != null) lock (gate) writer.WriteLine(ev.Data); };
    p.OutputDataReceived += h;
    p.ErrorDataReceived += h;
    p.EnableRaisingEvents = true;
    p.Exited += (s, ev) => { Thread.Sleep(300); lock (gate) writer.Dispose(); };
    p.Start();
    p.BeginOutputReadLine();
    p.BeginErrorReadLine();
    return p;
  }

  // Runs to the end (or the timeout: killed, -1). The exit status otherwise.
  public static int Run(string exe, string[] args, string cwd, Hashtable env, string outPath, int timeoutSeconds) {
    using (Process p = Start(exe, args, cwd, env, outPath)) {
      if (!p.WaitForExit(timeoutSeconds * 1000)) {
        try { p.Kill(); } catch (Exception) { }
        p.WaitForExit(10000);
        return -1;
      }
      p.WaitForExit();  // the output read to the end
      return p.ExitCode;
    }
  }

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern IntPtr OpenProcess(int access, bool inherit, int pid);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool OpenProcessToken(IntPtr process, int access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool GetTokenInformation(IntPtr token, int cls, out int info, int len, out int ret);
  [DllImport("kernel32.dll")]
  static extern bool CloseHandle(IntPtr h);

  // 1 elevated (administrator), 0 not, -1 unknown.
  public static int Elevated(int pid) {
    IntPtr h = OpenProcess(0x1000 /* QUERY_LIMITED_INFORMATION */, false, pid);
    if (h == IntPtr.Zero) return -1;
    try {
      IntPtr token;
      if (!OpenProcessToken(h, 0x8 /* TOKEN_QUERY */, out token)) return -1;
      try {
        int elevated, ret;
        if (!GetTokenInformation(token, 20 /* TokenElevation */, out elevated, 4, out ret)) return -1;
        return elevated != 0 ? 1 : 0;
      } finally { CloseHandle(token); }
    } finally { CloseHandle(h); }
  }

  // ---- windows -------------------------------------------------------------------------------------------------
  delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] static extern int GetWindowThreadProcessId(IntPtr hwnd, out int pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr hwnd, int cmd);
  [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
  [DllImport("user32.dll")] static extern bool BringWindowToTop(IntPtr hwnd);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd, int msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

  static KufAcc() { SetProcessDPIAware(); }  // window rectangles and the cursor in real pixels

  // The process's largest visible top-level window without an owner (its main window), or zero.
  public static IntPtr MainWindow(int pid) {
    IntPtr best = IntPtr.Zero;
    long area = -1;
    EnumWindows((h, lp) => {
      int owner;
      GetWindowThreadProcessId(h, out owner);
      if (owner != pid || !IsWindowVisible(h) || GetWindow(h, 4 /* GW_OWNER */) != IntPtr.Zero) return true;
      RECT r;
      GetWindowRect(h, out r);
      long a = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
      if (a > area) { area = a; best = h; }
      return true;
    }, IntPtr.Zero);
    return best;
  }

  public static IntPtr WaitMainWindow(int pid, int seconds) {
    var sw = Stopwatch.StartNew();
    while (sw.Elapsed.TotalSeconds < seconds) {
      IntPtr h = MainWindow(pid);
      if (h != IntPtr.Zero) return h;
      Thread.Sleep(250);
    }
    return IntPtr.Zero;
  }

  public static int ForegroundPid() {
    int pid;
    GetWindowThreadProcessId(GetForegroundWindow(), out pid);
    return pid;
  }

  public static string ForegroundTitle() {
    var sb = new StringBuilder(256);
    GetWindowText(GetForegroundWindow(), sb, 256);
    return sb.ToString();
  }

  public static int[] Rect(IntPtr h) {
    RECT r;
    GetWindowRect(h, out r);
    return new[] { r.Left, r.Top, r.Right, r.Bottom };
  }

  // Brings a window to the front: an Alt press first (Windows lets the process that last had input set the
  // foreground window).
  public static bool Focus(IntPtr h) {
    Key(0x12, false);
    Key(0x12, true);
    ShowWindow(h, 9 /* SW_RESTORE */);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    Thread.Sleep(300);
    return GetForegroundWindow() == h;
  }

  public static void Close(IntPtr h) { PostMessage(h, 0x0010 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }

  public static bool Screenshot(IntPtr h, string path) {
    try {
      int[] r = Rect(h);
      int w = Math.Max(1, r[2] - r[0]), hh = Math.Max(1, r[3] - r[1]);
      using (var bmp = new Bitmap(w, hh))
      using (var g = Graphics.FromImage(bmp)) {
        g.CopyFromScreen(r[0], r[1], 0, 0, new Size(w, hh));
        bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
      }
      return true;
    } catch (Exception) {
      return false;
    }
  }

  // ---- keyboard and mouse (SendInput: as a person's) ---------------------------------------------------------------
  [StructLayout(LayoutKind.Sequential)]
  struct MOUSEINPUT { public int dx, dy, mouseData, dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)]
  struct KEYBDINPUT { public short wVk, wScan; public int dwFlags, time; public IntPtr extra; }
  [StructLayout(LayoutKind.Explicit)]
  struct INPUTUNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
  [StructLayout(LayoutKind.Sequential)]
  struct INPUT { public int type; public INPUTUNION u; }
  [DllImport("user32.dll", SetLastError = true)] static extern int SendInput(int n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);

  static void Key(int vk, bool up) {
    var i = new INPUT { type = 1 };
    i.u.ki = new KEYBDINPUT { wVk = (short)vk, dwFlags = up ? 2 : 0 };
    SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
  }

  // Alt+Tab once: to the window used before (from the game to the desktop's last window, and back).
  public static void AltTab() {
    Key(0x12, false);
    Thread.Sleep(60);
    Key(0x09, false);
    Thread.Sleep(60);
    Key(0x09, true);
    Thread.Sleep(120);
    Key(0x12, true);
  }

  static void Mouse(int flags, int x, int y) {
    var i = new INPUT { type = 0 };
    int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
    i.u.mi = new MOUSEINPUT {
      dx = (int)((x - vx) * 65535.0 / Math.Max(1, vw - 1)), dy = (int)((y - vy) * 65535.0 / Math.Max(1, vh - 1)),
      dwFlags = flags | 0x8000 /* ABSOLUTE */ | 0x4000 /* VIRTUALDESK */
    };
    SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
  }

  public static void MoveCursor(int x, int y) { SetCursorPos(x, y); }

  // ---- drag and drop (OLE, as from Explorer) --------------------------------------------------------------------
  // Drags `paths` from a small helper window onto the point (x, y): the left button pressed on the helper, the
  // cursor moved to the point in steps, released there. The receiving window gets the drop through its OLE drop
  // target exactly as from Explorer. Returns the drag's result ("Copy", "None", ...).
  public static string DragFiles(string[] paths, int x, int y) {
    string result = "not run";
    var t = new Thread(() => {
      var form = new Form();
      form.FormBorderStyle = FormBorderStyle.None;
      form.ShowInTaskbar = false;
      form.TopMost = true;
      form.StartPosition = FormStartPosition.Manual;
      form.BackColor = Color.OrangeRed;
      form.Size = new Size(64, 64);
      int sx = Math.Max(0, x - 400), sy = y;
      form.Location = new Point(sx - 32, sy - 32);
      form.Shown += (s, e) => {
        form.Activate();
        Thread.Sleep(300);
        SetCursorPos(sx, sy);
        Thread.Sleep(150);
        Mouse(0x0002 /* LEFTDOWN */, sx, sy);
        var mover = new Thread(() => {
          Thread.Sleep(400);
          for (int k = 1; k <= 30; k++) {
            Mouse(0x0001 /* MOVE */, sx + (x - sx) * k / 30, sy + (y - sy) * k / 30);
            Thread.Sleep(30);
          }
          Thread.Sleep(500);
          Mouse(0x0001, x + 1, y);
          Thread.Sleep(300);
          Mouse(0x0004 /* LEFTUP */, x + 1, y);
        });
        mover.IsBackground = true;
        mover.Start();
        var files = new StringCollection();
        files.AddRange(paths);
        var data = new DataObject();
        data.SetFileDropList(files);
        result = form.DoDragDrop(data, DragDropEffects.Copy | DragDropEffects.Link | DragDropEffects.Move).ToString();
        mover.Join(5000);
        form.Close();
      };
      Application.Run(form);
    });
    t.SetApartmentState(ApartmentState.STA);
    t.Start();
    t.Join(30000);
    return result;
  }

  // ---- controllers (XInput) ------------------------------------------------------------------------------------
  [StructLayout(LayoutKind.Sequential)]
  struct XGAMEPAD { public ushort buttons; public byte lt, rt; public short lx, ly, rx, ry; }
  [StructLayout(LayoutKind.Sequential)]
  struct XSTATE { public uint packet; public XGAMEPAD pad; }
  [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")] static extern int XInputGetState(int user, out XSTATE s);

  // Per XInput slot 0-3: -1 not connected, else the pressed buttons (XINPUT_GAMEPAD_* bits).
  public static int[] Pads() {
    var r = new int[4];
    for (int k = 0; k < 4; k++) {
      XSTATE s;
      try { r[k] = XInputGetState(k, out s) == 0 ? s.pad.buttons : -1; } catch (Exception) { r[k] = -1; }
    }
    return r;
  }
}
