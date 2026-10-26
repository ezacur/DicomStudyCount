// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the columns of an Explorer window (IColumnManager): the Comment column, the DICOM ones

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Explorer4Dicoms
{
    // Adds the Comments column to an Explorer window that is already open
    // (ShellWindows -> IShellBrowser -> IShellView -> IColumnManager)
    public static class ExplorerColumns
    {
        [ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IComServiceProvider
        {
            void QueryService(ref Guid guidService, ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out object ppv);
        }

        [ComImport, Guid("000214E2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellBrowser
        {
            void GetWindow(out IntPtr phwnd);
            void ContextSensitiveHelp(bool fEnterMode);
            void InsertMenusSB(IntPtr hmenuShared, IntPtr lpMenuWidths);
            void SetMenuSB(IntPtr hmenuShared, IntPtr holemenuRes, IntPtr hwndActiveObject);
            void RemoveMenusSB(IntPtr hmenuShared);
            void SetStatusTextSB(IntPtr pszStatusText);
            void EnableModelessSB(bool fEnable);
            void TranslateAcceleratorSB(IntPtr pmsg, ushort wID);
            void BrowseObject(IntPtr pidl, uint wFlags);
            void GetViewStateStream(uint grfMode, out IntPtr ppStrm);
            void GetControlWindow(uint id, out IntPtr phwnd);
            void SendControlMsg(uint id, uint uMsg, IntPtr wParam, IntPtr lParam, out IntPtr pret);
            void QueryActiveShellView([MarshalAs(UnmanagedType.Interface)] out object ppshv);
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PROPERTYKEY { public Guid fmtid; public uint pid; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct CM_COLUMNINFO
        {
            public uint cbSize, dwMask, dwState, uWidth, uDefaultWidth, uIdealWidth;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 80)] public string wszName;
        }

        [ComImport, Guid("D8EC27BB-3F3B-4042-B10A-4ACFD924D453"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IColumnManager
        {
            void SetColumnInfo(ref PROPERTYKEY propkey, ref CM_COLUMNINFO pcmci);
            void GetColumnInfo(ref PROPERTYKEY propkey, ref CM_COLUMNINFO pcmci);
            void GetColumnCount(uint dwFlags, out uint puCount);
            void GetColumns(uint dwFlags, [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 2)] PROPERTYKEY[] rgkeyOrder, uint cColumns);
            void SetColumns([In, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 1)] PROPERTYKEY[] rgkeyOrder, uint cVisible);
        }

        static readonly Guid SID_STopLevelBrowser = new Guid("4C96BE40-915C-11CF-99D3-00AA004AE837");
        static readonly Guid PKEY_Comment_fmtid = new Guid("F29F85E0-4FF9-1068-AB91-08002B27B3D9");
        const uint PKEY_Comment_pid = 6;
        const uint CM_ENUM_VISIBLE = 0x2;
        const uint CM_MASK_WIDTH = 0x1;
        const uint CM_WIDTH_AUTOSIZE = 0xFFFFFFFE;     // -2: width fitted to the content

        // Adds the Comments column as the last column if missing and fits its width to the text.
        // Returns true if it was added.
        public static bool ShowCommentColumn(object explorerWindow)
        {
            Guid sid = SID_STopLevelBrowser, iid = typeof(IShellBrowser).GUID;
            object browser;
            ((IComServiceProvider)explorerWindow).QueryService(ref sid, ref iid, out browser);
            object view;
            ((IShellBrowser)browser).QueryActiveShellView(out view);
            var cm = (IColumnManager)view;

            uint count;
            cm.GetColumnCount(CM_ENUM_VISIBLE, out count);
            var keys = new PROPERTYKEY[count];
            cm.GetColumns(CM_ENUM_VISIBLE, keys, count);
            var comment = new PROPERTYKEY { fmtid = PKEY_Comment_fmtid, pid = PKEY_Comment_pid };
            bool present = false;
            foreach (var k in keys)
                if (k.fmtid == PKEY_Comment_fmtid && k.pid == PKEY_Comment_pid) present = true;

            if (!present)
            {
                var list = new List<PROPERTYKEY>(keys);
                list.Add(comment);     // as the last column
                cm.SetColumns(list.ToArray(), (uint)list.Count);
            }

            var info = new CM_COLUMNINFO
            {
                cbSize = (uint)Marshal.SizeOf(typeof(CM_COLUMNINFO)),
                dwMask = CM_MASK_WIDTH,
                uWidth = CM_WIDTH_AUTOSIZE,
                wszName = ""
            };
            cm.SetColumnInfo(ref comment, ref info);
            return !present;
        }

        [DllImport("propsys.dll", CharSet = CharSet.Unicode)]
        static extern int PSGetPropertyKeyFromName(string name, out PROPERTYKEY key);

        static IColumnManager ColumnsOf(object explorerWindow)
        {
            Guid sid = SID_STopLevelBrowser, iid = typeof(IShellBrowser).GUID;
            object browser;
            ((IComServiceProvider)explorerWindow).QueryService(ref sid, ref iid, out browser);
            object view;
            ((IShellBrowser)browser).QueryActiveShellView(out view);
            return (IColumnManager)view;
        }

        static List<PROPERTYKEY> Visible(IColumnManager cm)
        {
            uint count;
            cm.GetColumnCount(CM_ENUM_VISIBLE, out count);
            var keys = new PROPERTYKEY[count];
            cm.GetColumns(CM_ENUM_VISIBLE, keys, count);
            return new List<PROPERTYKEY>(keys);
        }

        // The keys of the properties named (those not registered are left out)
        static List<PROPERTYKEY> KeysOf(string[] names)
        {
            var keys = new List<PROPERTYKEY>();
            foreach (var n in names)
            {
                PROPERTYKEY k;
                if (PSGetPropertyKeyFromName(n, out k) == 0) keys.Add(k);
            }
            return keys;
        }

        static bool Contains(List<PROPERTYKEY> keys, PROPERTYKEY k)
        {
            foreach (var x in keys) if (x.fmtid == k.fmtid && x.pid == k.pid) return true;
            return false;
        }

        // True if any of the columns named is visible in the window
        public static bool AnyColumnShown(object explorerWindow, string[] names)
        {
            var visible = Visible(ColumnsOf(explorerWindow));
            foreach (var k in KeysOf(names)) if (Contains(visible, k)) return true;
            return false;
        }

        // Shows the columns named (after the visible ones, in that order, fitted to their content) or
        // hides them; the other columns stay as they are
        public static void SetColumnsShown(object explorerWindow, string[] names, bool show)
        {
            var cm = ColumnsOf(explorerWindow);
            var wanted = KeysOf(names);
            var columns = new List<PROPERTYKEY>();
            foreach (var k in Visible(cm)) if (!Contains(wanted, k)) columns.Add(k);
            if (show) columns.AddRange(wanted);
            cm.SetColumns(columns.ToArray(), (uint)columns.Count);
            if (!show) return;
            foreach (var k in wanted)
            {
                var key = k;
                var info = new CM_COLUMNINFO
                {
                    cbSize = (uint)Marshal.SizeOf(typeof(CM_COLUMNINFO)),
                    dwMask = CM_MASK_WIDTH,
                    uWidth = CM_WIDTH_AUTOSIZE,
                    wszName = ""
                };
                cm.SetColumnInfo(ref key, ref info);
            }
        }

        // Removes the Comments column from the view. Returns true if it was visible.
        public static bool HideCommentColumn(object explorerWindow)
        {
            Guid sid = SID_STopLevelBrowser, iid = typeof(IShellBrowser).GUID;
            object browser;
            ((IComServiceProvider)explorerWindow).QueryService(ref sid, ref iid, out browser);
            object view;
            ((IShellBrowser)browser).QueryActiveShellView(out view);
            var cm = (IColumnManager)view;

            uint count;
            cm.GetColumnCount(CM_ENUM_VISIBLE, out count);
            var keys = new PROPERTYKEY[count];
            cm.GetColumns(CM_ENUM_VISIBLE, keys, count);
            var list = new List<PROPERTYKEY>();
            foreach (var k in keys)
                if (!(k.fmtid == PKEY_Comment_fmtid && k.pid == PKEY_Comment_pid)) list.Add(k);
            if (list.Count == keys.Length) return false;
            cm.SetColumns(list.ToArray(), (uint)list.Count);
            return true;
        }
    }
}
