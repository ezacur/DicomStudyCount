// explorer4dicoms, the C# part of explorer4dicoms.ps1 (bin\Explorer4Dicoms.dll, built by tools\Build.ps1 from
// all of src\Explorer4Dicoms\): what a count finds: the studies of a folder, the splits, the duplicates, the incomplete series

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
    public class Study
    {
        public string StudyDate { get; set; }
        public string Modalities { get; set; }
        public string PatientID { get; set; }
        public string StudyDescription { get; set; }
        public int Series { get; set; }
        public int Files { get; set; }
        public string StudyInstanceUID { get; set; }
    }

    public class ScanProgress
    {
        public long TotalFiles;
        public long Files;      // files processed
        public long Dicom;      // DICOM with StudyInstanceUID
        public long NoStudy;    // DICOM without StudyInstanceUID
        public long NotDicom;
        public long Errors;
        public int Studies;
        public long CommentErrors;
        public volatile bool CountingShown;     // "Counting..." already written and visible in Explorer
        public List<string> InaccessibleDirs = new List<string>();
    }

    public class FolderStudies
    {
        public string Path { get; set; }
        public int Studies { get; set; }
        public int Series { get; set; }
        public int Files { get; set; }
        public string StudyInstanceUID { get; set; }   // only when the folder holds a single study
    }

    // A study found in more than one separate folder (its topmost single-study folders)
    public class StudySplit
    {
        public string Tag { get; set; }
        public string StudyInstanceUID { get; set; }
        public List<string> Folders { get; set; }
    }

    // Files sharing one SOPInstanceUID: identical copies (same bytes) or re-processed versions
    public class DuplicateGroup
    {
        public bool Identical { get; set; }
        public List<string> Files { get; set; }
    }

    // Files with the same SOPInstanceUID (the same image) found in several folders,
    // grouped by the set of folders involved
    public class DuplicateSet
    {
        public int Files { get; set; }
        public int Identical { get; set; }      // files in groups of identical copies
        public int Different { get; set; }      // files in groups whose contents differ
        public List<string> Folders { get; set; }
        public string Keep { get; set; }        // suggested folder to keep the copies in
        public List<DuplicateGroup> Groups { get; set; }
    }

    public class ScanResult
    {
        public ScanProgress Counts;
        public List<Study> Studies;
        public List<FolderStudies> Folders;
        public List<StudySplit> Splits;
        public List<DuplicateSet> Duplicates;
        public int DuplicatedFiles;      // files that have a copy elsewhere (all copies counted)
        public List<IncompleteSeries> Incomplete;
        public List<ExternalCopySet> External;
    }

    // A series that, in this folder, lacks some of the InstanceNumbers seen for it in the whole tree
    public class IncompleteSeries
    {
        public string Tag { get; set; }         // study tag
        public string Series { get; set; }      // "<SeriesNumber>.<SeriesDescription>"
        public string Folder { get; set; }
        public int Have { get; set; }
        public int Expected { get; set; }
        public string Missing { get; set; }     // missing InstanceNumbers, as ranges ("4, 7-9, ...")
    }

    // Byte-identical copies (or same study + SOPInstanceUID with different contents) found in another
    // counted location, grouped by this folder and the other folder
    public class ExternalCopySet
    {
        public int Files { get; set; }
        public int Identical { get; set; }
        public int Different { get; set; }
        public string Folder { get; set; }
        public string OtherFolder { get; set; }
    }
}
