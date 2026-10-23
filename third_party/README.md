# third_party

Code of others, copied verbatim: do not edit. Only needed to build `bin\DicomShell.dll`
(`tools\Build.ps1`); using the tool needs nothing from here.

| Folder | What | Used for |
|---|---|---|
| `dcmtk-3.7.0` | DCMTK 3.7.0 (OFFIS), a subset of 7 modules: ofstd, oflog, dcmdata, dcmimgle, dcmimage, dcmjpeg (with the IJG JPEG libraries) and dcmjpls (with its own CharLS) | the preview of DICOM files: decoding the pixel data (uncompressed, JPEG baseline / extended / lossless, JPEG-LS, RLE), rendering it, and the names of the header attributes |

`dcmtk-3.7.0` is the same subset used by `C:\repos\I3Dfileformat` (copied from its `third_party` on
2026-10-23, identical byte for byte); see its `ORIGIN.txt` for where it comes from and what was
left out. Licence: BSD-style (`dcmtk-3.7.0\COPYRIGHT`), with the IJG and CharLS notices inside.
