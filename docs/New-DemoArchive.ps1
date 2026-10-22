<#
.SYNOPSIS
    Creates a synthetic DICOM archive (fake patients and UIDs) to try the tool and make screenshots.
.EXAMPLE
    .\New-DemoArchive.ps1 -Root D:\DICOM_DEMO
#>
param([Parameter(Mandatory = $true)][string]$Root)
# Synthetic DICOM archive for the README screenshots: fake patients and UIDs, but the same kinds of
# problems found in real data (split study, duplicated files, incomplete series)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System; using System.IO; using System.Text; using System.Collections.Generic;
public static class Demo {
    static Random rnd = new Random(42);
    public static string Uid() {
        var sb = new StringBuilder("2.25.");
        sb.Append(rnd.Next(1, 9));
        for (int i = 0; i < 30; i++) sb.Append(rnd.Next(0, 10));
        return sb.ToString();
    }
    static void W(Stream s, byte[] b) { s.Write(b, 0, b.Length); }
    static byte[] Pad(string v, bool ui) { if (v.Length % 2 == 1) v += ui ? "\0" : " "; return Encoding.ASCII.GetBytes(v); }
    static void Elem(Stream s, int g, int e, string vr, byte[] val) {
        W(s, BitConverter.GetBytes((ushort)g)); W(s, BitConverter.GetBytes((ushort)e));
        W(s, Encoding.ASCII.GetBytes(vr));
        if (vr == "OB" || vr == "OW") { W(s, new byte[2]); W(s, BitConverter.GetBytes((uint)val.Length)); }
        else W(s, BitConverter.GetBytes((ushort)val.Length));
        W(s, val);
    }
    static void Str(Stream s, int g, int e, string vr, string v) { Elem(s, g, e, vr, Pad(v, vr == "UI")); }
    public static void Write(string path, string sopClass, string sop, string date, string modality, string studyDesc,
                             string seriesDesc, string patient, string patientId, string study, string series,
                             int seriesNumber, int instance) {
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        using (var s = new FileStream(path, FileMode.Create)) {
            W(s, new byte[128]); W(s, Encoding.ASCII.GetBytes("DICM"));
            var meta = new MemoryStream();
            Str(meta, 2, 1, "OB", ""); meta.SetLength(0);
            Elem(meta, 2, 0x0001, "OB", new byte[] { 0, 1 });
            Str(meta, 2, 0x0002, "UI", sopClass);
            Str(meta, 2, 0x0003, "UI", sop);
            Str(meta, 2, 0x0010, "UI", "1.2.840.10008.1.2.1");
            Elem(s, 2, 0, "UL", BitConverter.GetBytes((uint)meta.Length));
            W(s, meta.ToArray());
            Str(s, 8, 0x0016, "UI", sopClass);
            Str(s, 8, 0x0018, "UI", sop);
            Str(s, 8, 0x0020, "DA", date);
            Str(s, 8, 0x0060, "CS", modality);
            Str(s, 8, 0x1030, "LO", studyDesc);
            Str(s, 8, 0x103E, "LO", seriesDesc);
            Str(s, 0x10, 0x0010, "PN", patient);
            Str(s, 0x10, 0x0020, "LO", patientId);
            Str(s, 0x20, 0x000D, "UI", study);
            Str(s, 0x20, 0x000E, "UI", series);
            Str(s, 0x20, 0x0011, "IS", seriesNumber.ToString());
            Str(s, 0x20, 0x0013, "IS", instance.ToString());
            var pixels = new byte[4096]; rnd.NextBytes(pixels);
            Elem(s, 0x7FE0, 0x0010, "OW", pixels);
        }
    }
}
'@

$ct = '1.2.840.10008.5.1.4.1.1.2'; $mr = '1.2.840.10008.5.1.4.1.1.4'
function New-Study($folder, $modality, $date, $desc, $patient, $id, $seriesList) {
    $study = [Demo]::Uid()
    $sopClass = if ($modality -eq 'CT') { $ct } else { $mr }
    $n = 0
    foreach ($s in $seriesList) {
        $n++
        $series = [Demo]::Uid()
        for ($i = 1; $i -le $s.Images; $i++) {
            $file = Join-Path $Root ("{0}\S{1:D3}\IM{2:D4}" -f $folder, $s.Number, $i)
            [Demo]::Write($file, $sopClass, [Demo]::Uid(), $date, $modality, $desc, $s.Name, $patient, $id, $study, $series, $s.Number, $i)
        }
    }
}
function S($number, $name, $images) { [pscustomobject]@{ Number = $number; Name = $name; Images = $images } }

if (Test-Path $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
New-Study 'CASE_0001\DICOM.CT' 'CT' '20250314' 'CARDIAC CT' 'DEMO^ALPHA' 'D0001' @((S 1 'Topogram' 2), (S 2 'CorCTA 0.75 Bv40 75%' 60), (S 3 'Calcium Score' 40))
New-Study 'CASE_0001\DICOM.MR' 'MR' '20250316' 'CARDIAC MR' 'DEMO^ALPHA' 'D0001' @((S 1 'localizer' 3), (S 7 'cine_retro_4ch' 25), (S 8 'cine_retro_sax' 30), (S 12 'LGE_PSIR' 20))
New-Study 'CASE_0002\DICOM.MR' 'MR' '20250602' 'CARDIAC MR' 'DEMO^BRAVO' 'D0002' @((S 1 'localizer' 3), (S 7 'cine_retro_4ch' 25), (S 12 'LGE_PSIR' 20), (S 15 'T1_map' 12))
New-Study 'CASE_0003\DICOM.CT' 'CT' '20250910' 'CARDIAC CT' 'DEMO^CHARLIE' 'D0003' @((S 2 'CorCTA 0.75 Bv40 70%' 60), (S 4 'CorCTA 0.75 Bv40 40%' 60))

# Problems: part of CASE_0002's MR copied into CASE_0003 (split study + duplicates),
# and one of the copied series only half copied (incomplete series)
$src = Join-Path $Root 'CASE_0002\DICOM.MR'
$dst = Join-Path $Root 'CASE_0003\MR_copy'
New-Item -ItemType Directory -Force "$dst\S007", "$dst\S012" | Out-Null
Copy-Item "$src\S007\*" "$dst\S007"
Get-ChildItem "$src\S012" | Sort-Object Name | Select-Object -First 9 | Copy-Item -Destination "$dst\S012"
"demo files: " + @(Get-ChildItem $Root -Recurse -File).Count
