// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the folder icons in the colors of the studies they hold

namespace Explorer4Dicoms
{
    using System;
    using System.Collections.Generic;
    using System.Drawing;
    using System.Drawing.Drawing2D;
    using System.Drawing.Imaging;
    using System.IO;
    using System.Runtime.InteropServices;

    // Folder icons in the colors of the studies they hold, for the Name column of Explorer (desktop.ini
    // IconResource): an archive box instead of a folder; one study -> a box of its color; 2 to 5 ->
    // vertical stripes; more -> horizontal rainbow stripes (the problems are marked in the comment
    // only). The file name says what the icon is ("c05.ico", "c05-17.ico", "rainbow.ico"), so a lost
    // icon is rebuilt from its name alone.
    public static class FolderIcons
    {
        // Colors chosen to be told apart at 16 px. No yellow: it would look like a plain folder, and
        // it is one of the rainbow stripes.
        public static readonly int[] Palette =
        {
            0x000000, 0x404040, 0x808080, 0xBFBFBF, 0xFFFFFF,   // black, gray 25/50/75 %, white
            0xE6194B, 0x800000, 0xF58231, 0xFFD8B1, 0x9A6324,   // red, maroon, orange, apricot, brown
            0x808000, 0xBFEF45, 0x3CB44B, 0xAAFFC3, 0x469990,   // olive, lime, green, mint, teal
            0x42D4F4, 0x4363D8, 0x000075, 0xDCBEFF, 0x911EB4,   // cyan, blue, navy, lavender, purple
            0xF032E6, 0xFABED4                                  // magenta, pink
        };

        public const int MaxStripes = 5;
        static readonly int[] Sizes = { 16, 20, 24, 32, 40, 48, 64, 256 };

        // Versioned, so a change in the drawing never mixes with icons already cached by Explorer
        public static readonly string Dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"explorer4dicoms\icons");
        static readonly string VersionDir = Path.Combine(Dir, "v2");

        // Always the same color for the same study (64-bit FNV-1a of the StudyInstanceUID)
        public static int ColorIndex(string uid)
        {
            ulong h = 14695981039346656037;
            foreach (char c in uid) { h ^= c; h *= 1099511628211; }
            return (int)((h >> 32) % (ulong)Palette.Length);
        }

        // Icon file name for the color indexes of the studies of a folder (in display order)
        public static string NameFor(IList<int> colors)
        {
            if (colors.Count == 0) return null;
            if (colors.Count > MaxStripes) return "rainbow.ico";
            var parts = new string[colors.Count];
            for (int i = 0; i < colors.Count; i++) parts[i] = colors[i].ToString("D2");
            return "c" + string.Join("-", parts) + ".ico";
        }

        // Also the icons written by another user or on another PC (other profile folder), so a count
        // there replaces them instead of keeping them as someone else's custom icon
        public static bool IsOurs(string iconPath)
        {
            return !string.IsNullOrEmpty(iconPath) &&
                   (iconPath.IndexOf(@"\explorer4dicoms\icons\", StringComparison.OrdinalIgnoreCase) >= 0 ||
                    iconPath.IndexOf(@"\DicomStudyCount\icons\", StringComparison.OrdinalIgnoreCase) >= 0);     // (the name of the suite before)
        }

        // Full path of the icon, created if it does not exist yet (or was lost)
        public static string Ensure(string name)
        {
            string path = Path.Combine(VersionDir, name);
            if (File.Exists(path)) return path;
            // "-w": the warning sign of earlier versions, no longer drawn
            bool warning = name.EndsWith("-w.ico", StringComparison.OrdinalIgnoreCase);
            string body = name.Substring(0, name.Length - (warning ? 6 : 4));
            List<int> colors = null;     // null = rainbow
            if (body != "rainbow")
            {
                if (!body.StartsWith("c")) throw new ArgumentException("Not an icon name: " + name);
                colors = new List<int>();
                foreach (var part in body.Substring(1).Split('-'))
                {
                    int i = int.Parse(part);
                    if (i < 0 || i >= Palette.Length) throw new ArgumentException("Not an icon name: " + name);
                    colors.Add(i);
                }
            }
            Directory.CreateDirectory(VersionDir);
            // Written under a temporary name and renamed: several threads (or counts) may build it at once
            string tmp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            WriteIco(tmp, colors);
            try { File.Move(tmp, path); }
            catch (IOException) { File.Delete(tmp); }     // built by someone else meanwhile
            return path;
        }

        // Icon file of a desktop.ini IconResource value ("path,index")
        public static string PathOf(string iconResource)
        {
            if (string.IsNullOrEmpty(iconResource)) return "";
            int comma = iconResource.LastIndexOf(',');
            return comma > 0 ? iconResource.Substring(0, comma) : iconResource;
        }

        // Icon file of a folder written by this script, rebuilt first if it was lost; null if the
        // folder has no icon of ours
        public static string OfFolder(string dir)
        {
            string ini = Path.Combine(dir, "desktop.ini");
            if (!File.Exists(ini)) return null;
            string path = PathOf(Native.ReadIni(ini, ".ShellClassInfo", "IconResource"));
            if (!IsOurs(path)) return null;
            if (!File.Exists(path) &&
                string.Equals(Path.GetDirectoryName(path), VersionDir, StringComparison.OrdinalIgnoreCase))
            {
                try { Ensure(Path.GetFileName(path)); }
                catch (Exception) { return null; }
                Native.NotifyUpdated(dir);
            }
            return File.Exists(path) ? path : null;
        }

        // The image of one of our .ico files at that size (the nearest larger one, scaled down)
        public static Bitmap Load(string path, int size)
        {
            byte[] ico = File.ReadAllBytes(path);
            int count = BitConverter.ToInt16(ico, 4), best = -1, bestSide = int.MaxValue, largest = -1, largestSide = 0;
            for (int i = 0; i < count; i++)
            {
                int side = ico[6 + 16 * i] == 0 ? 256 : ico[6 + 16 * i];
                if (side >= size && side < bestSide) { best = i; bestSide = side; }
                if (side > largestSide) { largest = i; largestSide = side; }
            }
            if (best < 0) best = largest;
            int length = BitConverter.ToInt32(ico, 6 + 16 * best + 8), offset = BitConverter.ToInt32(ico, 6 + 16 * best + 12);
            using (var png = new Bitmap(new MemoryStream(ico, offset, length)))
            {
                var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
                using (var g = Graphics.FromImage(bmp))
                {
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.DrawImage(png, 0, 0, size, size);
                }
                return bmp;
            }
        }

        // Rebuilds the lost icons of the subfolders of a folder (what Explorer shows when browsing it)
        public static void RepairChildren(string dir)
        {
            int n = 0;
            foreach (var sub in Directory.EnumerateDirectories(dir))
            {
                if (n++ >= 2000) break;
                OfFolder(sub);
            }
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern int SHDefExtractIcon(string file, int index, uint flags, out IntPtr large, out IntPtr small, uint size);

        [DllImport("user32.dll")]
        static extern bool DestroyIcon(IntPtr h);

        // The standard folder icon of Windows at that size (imageres.dll, resource 3)
        public static Bitmap SystemFolder(int size)
        {
            IntPtr large, small;
            string imageres = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "imageres.dll");
            if (SHDefExtractIcon(imageres, -3, 0, out large, out small, (uint)size) != 0 || large == IntPtr.Zero)
                throw new InvalidOperationException("Folder icon not found");
            try
            {
                using (var icon = Icon.FromHandle(large))
                using (var bmp = icon.ToBitmap())
                    return new Bitmap(bmp);
            }
            finally { DestroyIcon(large); }
        }

        static void WriteIco(string path, List<int> colors)
        {
            var pngs = new List<byte[]>();
            foreach (int size in Sizes)
                using (var bmp = Draw(size, colors))
                using (var ms = new MemoryStream())
                {
                    bmp.Save(ms, ImageFormat.Png);
                    pngs.Add(ms.ToArray());
                }
            // .ico with PNG entries (supported since Windows Vista)
            using (var w = new BinaryWriter(new FileStream(path, FileMode.CreateNew)))
            {
                w.Write((short)0); w.Write((short)1); w.Write((short)Sizes.Length);
                int offset = 6 + 16 * Sizes.Length;
                for (int i = 0; i < Sizes.Length; i++)
                {
                    byte side = (byte)(Sizes[i] >= 256 ? 0 : Sizes[i]);
                    w.Write(side); w.Write(side); w.Write((byte)0); w.Write((byte)0);
                    w.Write((short)1); w.Write((short)32);
                    w.Write(pngs[i].Length); w.Write(offset);
                    offset += pngs[i].Length;
                }
                foreach (var p in pngs) w.Write(p);
            }
        }

        // An archive box (lid, handle and, from 32 px, a label) drawn on a 16x16 grid scaled to the size:
        // its own shape, so a box of studies is never taken for a plain folder whatever its color
        public static Bitmap Draw(int size, List<int> colors)
        {
            var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb);
            float s = size / 16f;
            using (var g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                g.ScaleTransform(s, s);
                var bands = Bands(colors, s);
                Paint(g, s, Rounded(1.5f, 5.5f, 13, 9, 0.7f), bands, 0f);
                Paint(g, s, Rounded(0.6f, 2.8f, 14.8f, 3.6f, 0.6f), bands, colors == null ? -0.08f : -0.18f);

                // Handle: a slot darker than the box (lighter on dark colors)
                float at = colors == null ? 8.55f : 8f;
                Color mid = bands.Find(x => x.From <= at && at < x.To).Color;
                using (var b = new SolidBrush(Lum(mid) < 0.25f ? Color.FromArgb(150, 150, 150) : Mix(mid, Color.Black, 0.6f)))
                    g.FillPath(b, Rounded(5.8f, 7.8f, 4.4f, 1.5f, 0.75f));
                if (size >= 32)
                {
                    using (var b = new SolidBrush(Color.FromArgb(235, 255, 255, 255)))
                        g.FillPath(b, Rounded(4.2f, 10.6f, 7.6f, 2.4f, 0.3f));
                    using (var p = new Pen(Color.FromArgb(150, 120, 120, 120), 1f / s))
                        g.DrawPath(p, Rounded(4.2f, 10.6f, 7.6f, 2.4f, 0.3f));
                }
            }
            return bmp;
        }

        // A band of color: the area [From, To) of the grid, vertical (x) or horizontal (y)
        class Band { public float From, To; public Color Color; public bool Horizontal; }

        // The studies as vertical stripes, left to right; more than MaxStripes as horizontal rainbow
        // stripes, so "many" is told from "a few" by the direction and not only by the colors.
        // The edges fall on whole pixels, so the stripes stay sharp at 16 px.
        static List<Band> Bands(List<int> colors, float s)
        {
            var bands = new List<Band>();
            bool rainbow = colors == null;
            int n = rainbow ? Rainbow.Length : colors.Count;
            float from = rainbow ? 2.8f : 1.5f, to = rainbow ? 14.5f : 14.5f;
            for (int i = 0; i < n; i++)
            {
                float a = (float)Math.Round((from + (to - from) * i / n) * s) / s;
                float z = (float)Math.Round((from + (to - from) * (i + 1) / n) * s) / s;
                bands.Add(new Band
                {
                    From = i == 0 ? -1 : a, To = i == n - 1 ? 17 : z, Horizontal = rainbow,
                    Color = rainbow ? Rainbow[i] : FromRgb(Palette[colors[i]])
                });
            }
            return bands;
        }

        static readonly Color[] Rainbow =
        {
            Color.FromArgb(230, 25, 40), Color.FromArgb(250, 135, 20), Color.FromArgb(250, 215, 0),
            Color.FromArgb(40, 170, 60), Color.FromArgb(30, 110, 230), Color.FromArgb(140, 50, 190)
        };

        // Fills a part of the box band by band, shaded (shade < 0: darker), with a 1 px edge in a
        // darker tone of each band (gray for white and for dark colors, visible on any background)
        static void Paint(Graphics g, float s, GraphicsPath path, List<Band> bands, float shade)
        {
            var edge = Inset(path, 0.5f / s);
            foreach (var band in bands)
            {
                g.SetClip(band.Horizontal ? new RectangleF(-1, band.From, 18, band.To - band.From)
                                          : new RectangleF(band.From, -1, band.To - band.From, 18));
                using (var b = new SolidBrush(Tone(band.Color, shade))) g.FillPath(b, path);
                using (var p = new Pen(Edge(band.Color), 1f / s)) g.DrawPath(p, edge);
                g.ResetClip();
            }
        }

        static GraphicsPath Rounded(float x, float y, float w, float h, float r)
        {
            var p = new GraphicsPath();
            float d = 2 * r;
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d, y, d, d, 270, 90);
            p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            p.AddArc(x, y + h - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // The same outline k units inside, so a 1 px edge falls on whole pixels
        static GraphicsPath Inset(GraphicsPath path, float k)
        {
            var b = path.GetBounds();
            var m = new Matrix(b, new[] { new PointF(b.X + k, b.Y + k), new PointF(b.Right - k, b.Y + k), new PointF(b.X + k, b.Bottom - k) });
            var copy = (GraphicsPath)path.Clone();
            copy.Transform(m);
            return copy;
        }

        static Color FromRgb(int rgb) { return Color.FromArgb((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }

        static float Lum(Color c) { return (0.299f * c.R + 0.587f * c.G + 0.114f * c.B) / 255f; }

        static Color Mix(Color a, Color b, float t)
        {
            return Color.FromArgb((int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));
        }

        // shade < 0: darker; very dark colors get lighter instead, where darker would not show
        static Color Tone(Color c, float shade)
        {
            if (shade == 0) return c;
            if (Lum(c) < 0.25f) return Mix(c, Color.White, -shade * 0.9f);
            return Mix(c, Color.Black, -shade);
        }

        static Color Edge(Color c)
        {
            float l = Lum(c);
            if (l > 0.8f) return Color.FromArgb(140, 140, 140);
            if (l < 0.25f) return Color.FromArgb(125, 125, 125);
            return Mix(c, Color.Black, 0.4f);
        }
    }
}
