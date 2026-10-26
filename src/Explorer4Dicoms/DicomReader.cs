// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): the DICOM header reader (Explicit/Implicit VR Little Endian, Big Endian, raw files without preamble)

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
    internal class DicomTags
    {
        public string Uid, SeriesUid, SeriesNumber, InstanceNumber, SopUid, Date, Modality, PatientID, Description, SeriesDescription;
    }

    internal static class DicomParser
    {
        const uint TagMediaStorageSOPClass = 0x00020002;
        const uint TagTransferSyntax = 0x00020010;
        const uint TagDirectoryRecordSequence = 0x00041220;
        const uint TagSOPClassUID = 0x00080016;
        const uint TagSOPInstanceUID = 0x00080018;
        const uint TagStudyDate = 0x00080020;
        const uint TagModality = 0x00080060;
        const uint TagStudyDescription = 0x00081030;
        const uint TagSeriesDescription = 0x0008103E;
        const uint TagPatientID = 0x00100020;
        const uint TagStudyInstanceUID = 0x0020000D;
        const uint TagSeriesInstanceUID = 0x0020000E;
        const uint TagSeriesNumber = 0x00200011;
        const uint TagInstanceNumber = 0x00200013;

        // SOP Class of a DICOMDIR (Media Storage Directory Storage)
        const string DirectorySopClass = "1.2.840.10008.1.3.10";

        static readonly Encoding Latin1 = Encoding.GetEncoding(28591);

        // null = not DICOM. Throws if it is DICOM but cannot be read. A DICOMDIR (the index of a
        // DICOM media, recognized by its contents, not by its name) is not counted either: null too.
        public static DicomTags ReadFile(string path)
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 4096))
            {
                var head = new byte[132];
                int n = ReadUpTo(fs, head);
                if (n == 132 && head[128] == 'D' && head[129] == 'I' && head[130] == 'C' && head[131] == 'M')
                {
                    fs.Position = 132;
                    return ReadAfterPreamble(fs);
                }

                // "Raw" DICOM without preamble: only attempted if it starts with group 0002 or 0008
                if (n < 8 || head[1] != 0 || (head[0] != 0x02 && head[0] != 0x08)) return null;
                fs.Position = 0;
                try
                {
                    var t = ReadAfterPreamble(fs);
                    return t == null || string.IsNullOrEmpty(t.Uid) ? null : t;
                }
                catch (Exception) { return null; }
            }
        }

        static DicomTags ReadAfterPreamble(FileStream fs)
        {
            // File meta information (group 0002): always Explicit VR Little Endian
            var r = new DicomReader(fs) { Explicit = true };
            string ts = null, mediaClass = null;
            while (fs.Position + 8 <= fs.Length)
            {
                long pos = fs.Position;
                uint tag = r.ReadTag();
                if ((tag >> 16) != 0x0002) { fs.Position = pos; break; }
                string vr;
                uint len = r.ReadLength(tag, out vr);
                if (tag == TagTransferSyntax) ts = Str(r.Bytes(len));
                else if (tag == TagMediaStorageSOPClass) mediaClass = Str(r.Bytes(len));
                else r.Skip(len);
            }
            if (mediaClass == DirectorySopClass) return null;   // DICOMDIR: its records are not read

            switch (ts)
            {
                case "1.2.840.10008.1.2":       // Implicit VR Little Endian
                    r.Explicit = false;
                    break;
                case "1.2.840.10008.1.2.2":     // Explicit VR Big Endian
                    r.BigEndian = true;
                    break;
                case "1.2.840.10008.1.2.1.99":  // Deflated Explicit VR Little Endian
                    r = new DicomReader(new DeflateStream(fs, CompressionMode.Decompress, true)) { Explicit = true };
                    break;
                case null:                      // no meta information: guess by looking for a VR
                    r.Explicit = LooksExplicit(fs);
                    break;
            }
            return ReadDataset(r);
        }

        static DicomTags ReadDataset(DicomReader r)
        {
            var t = new DicomTags();
            while (!r.AtEnd)
            {
                uint tag = r.ReadTag();
                if (tag > TagInstanceNumber) break;   // tags are sorted: they will not appear anymore
                string vr;
                uint len = r.ReadLength(tag, out vr);
                // DICOMDIR without (or with a wrong) meta information: it has directory records
                if (tag == TagDirectoryRecordSequence) return null;
                if (len == 0xFFFFFFFF) { r.SkipSequence(vr); continue; }
                switch (tag)
                {
                    case TagSOPClassUID:
                        if (Str(r.Bytes(len)) == DirectorySopClass) return null;
                        break;
                    case TagSOPInstanceUID: t.SopUid = Str(r.Bytes(len)); break;
                    case TagStudyDate: t.Date = Str(r.Bytes(len)); break;
                    case TagModality: t.Modality = Str(r.Bytes(len)); break;
                    case TagStudyDescription: t.Description = Str(r.Bytes(len)); break;
                    case TagSeriesDescription: t.SeriesDescription = Str(r.Bytes(len)); break;
                    case TagPatientID: t.PatientID = Str(r.Bytes(len)); break;
                    case TagStudyInstanceUID: t.Uid = Str(r.Bytes(len)); break;
                    case TagSeriesInstanceUID: t.SeriesUid = Str(r.Bytes(len)); break;
                    case TagSeriesNumber: t.SeriesNumber = Str(r.Bytes(len)); break;
                    case TagInstanceNumber: t.InstanceNumber = Str(r.Bytes(len)); return t;
                    default: r.Skip(len); break;
                }
            }
            return t;
        }

        static bool LooksExplicit(FileStream fs)
        {
            long pos = fs.Position;
            var b = new byte[6];
            int n = ReadUpTo(fs, b);
            fs.Position = pos;
            return n == 6 && b[4] >= 'A' && b[4] <= 'Z' && b[5] >= 'A' && b[5] <= 'Z';
        }

        static int ReadUpTo(Stream s, byte[] b)
        {
            int off = 0;
            while (off < b.Length)
            {
                int r = s.Read(b, off, b.Length - off);
                if (r <= 0) break;
                off += r;
            }
            return off;
        }

        static string Str(byte[] b)
        {
            return Latin1.GetString(b).Trim('\0', ' ');
        }
    }

    internal sealed class DicomReader
    {
        readonly Stream s;
        readonly byte[] buf = new byte[8];
        public bool Explicit;
        public bool BigEndian;

        public DicomReader(Stream s) { this.s = s; }

        public bool AtEnd { get { return s.CanSeek && s.Position + 4 > s.Length; } }

        void ReadExact(byte[] b, int n)
        {
            int off = 0;
            while (off < n)
            {
                int r = s.Read(b, off, n - off);
                if (r <= 0) throw new EndOfStreamException();
                off += r;
            }
        }

        public uint U16()
        {
            ReadExact(buf, 2);
            return BigEndian ? (uint)((buf[0] << 8) | buf[1]) : (uint)(buf[0] | (buf[1] << 8));
        }

        public uint U32()
        {
            ReadExact(buf, 4);
            return BigEndian
                ? ((uint)buf[0] << 24) | ((uint)buf[1] << 16) | ((uint)buf[2] << 8) | buf[3]
                : buf[0] | ((uint)buf[1] << 8) | ((uint)buf[2] << 16) | ((uint)buf[3] << 24);
        }

        public uint ReadTag()
        {
            uint g = U16();
            uint e = U16();
            return (g << 16) | e;
        }

        public uint ReadLength(uint tag, out string vr)
        {
            vr = null;
            if (Explicit && (tag >> 16) != 0xFFFE)
            {
                ReadExact(buf, 2);
                vr = new string(new[] { (char)buf[0], (char)buf[1] });
                if (HasLongLength(vr)) { U16(); return U32(); }
                return U16();
            }
            return U32();
        }

        static bool HasLongLength(string vr)
        {
            switch (vr)
            {
                case "OB": case "OD": case "OF": case "OL": case "OV": case "OW":
                case "SQ": case "SV": case "UC": case "UN": case "UR": case "UT": case "UV":
                    return true;
                default:
                    return false;
            }
        }

        public byte[] Bytes(uint n)
        {
            if (n > (1 << 20)) throw new InvalidDataException("Value too long");
            var b = new byte[n];
            ReadExact(b, (int)n);
            return b;
        }

        public void Skip(uint n)
        {
            if (s.CanSeek)
            {
                if (s.Position + n > s.Length) throw new EndOfStreamException();
                s.Seek(n, SeekOrigin.Current);
                return;
            }
            var tmp = new byte[8192];
            long left = n;
            while (left > 0)
            {
                int r = s.Read(tmp, 0, (int)Math.Min(left, tmp.Length));
                if (r <= 0) throw new EndOfStreamException();
                left -= r;
            }
        }

        // Skips a sequence of undefined length (including nested items).
        // A UN of undefined length is always encoded as Implicit VR Little Endian.
        public void SkipSequence(string vr)
        {
            bool saveExplicit = Explicit, saveBigEndian = BigEndian;
            if (vr == "UN") { Explicit = false; BigEndian = false; }
            try
            {
                while (true)
                {
                    uint tag = ReadTag();
                    uint len = U32();
                    if (tag == 0xFFFEE0DD) return;                         // Sequence Delimitation
                    if (tag != 0xFFFEE000) throw new InvalidDataException("Invalid sequence item");
                    if (len == 0xFFFFFFFF) SkipItem(); else Skip(len);
                }
            }
            finally
            {
                Explicit = saveExplicit;
                BigEndian = saveBigEndian;
            }
        }

        void SkipItem()
        {
            while (true)
            {
                uint tag = ReadTag();
                if (tag == 0xFFFEE00D) { U32(); return; }                  // Item Delimitation
                string vr;
                uint len = ReadLength(tag, out vr);
                if (len == 0xFFFFFFFF) SkipSequence(vr); else Skip(len);
            }
        }
    }
}
