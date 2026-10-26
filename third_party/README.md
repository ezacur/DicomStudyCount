# third_party

Code of others, copied verbatim: do not edit. Only needed to build `bin\DicomShell.dll`
(`tools\Build.ps1`); using the tool needs nothing from here.

| Folder | What | Used for |
|---|---|---|
| `dcmtk-3.7.0` | DCMTK 3.7.0 (OFFIS), a subset of 7 modules: ofstd, oflog, dcmdata, dcmimgle, dcmimage, dcmjpeg (with the IJG JPEG libraries) and dcmjpls (with its own CharLS) | the preview of DICOM files: decoding the pixel data (uncompressed, JPEG baseline / extended / lossless, JPEG-LS, RLE), rendering it, and the names of the header attributes |

| `openjpeg-2.5.3` | OpenJPEG 2.5.3 (uclouvain), only its library `openjp2` and the CMake files it needs | decoding JPEG 2000 and High-Throughput JPEG 2000 pixel data, which DCMTK cannot |
| `zlib-1.3.1` | zlib 1.3.1 (Jean-loup Gailly and Mark Adler), its library and the CMake files it needs | DCMTK reading deflated datasets, and the deflated image frames DicomShell decodes itself |

`openjpeg-2.5.3` comes from the release tarball `https://github.com/uclouvain/openjpeg/archive/refs/tags/v2.5.3.tar.gz`
(SHA-256 `368fe0468228e767433c9ebdea82ad9d801a3ad1e4234421f352c8b06e7aa707`, downloaded 2026-10-24),
with only these parts of it, unchanged: the top-level files (`CMakeLists.txt`, `LICENSE`, `*.md`,
`CTestConfig.cmake`), `cmake\`, `src\CMakeLists.txt`, `src\lib\CMakeLists.txt`, `src\lib\openjp2\` and
`wrapping\CMakeLists.txt`. Left out: the command-line tools and their libraries (`src\bin`,
`thirdparty`), JPIP, Java, tests and documentation. Licence: BSD 2-clause (`openjpeg-2.5.3\LICENSE`).

`zlib-1.3.1` comes from the release tarball `https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz`
(SHA-256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`, the published one, downloaded
2026-10-26), with only these parts of it, unchanged: `CMakeLists.txt`, `LICENSE`, `README`, `ChangeLog`,
`zconf.h.cmakein`, `zlib.pc.cmakein`, `zlib.map`, the sources (`*.c`, `*.h` but `zconf.h`, which its CMake
makes from `zconf.h.cmakein` and would otherwise rename in this folder) and `win32\`. Left out: `contrib`,
`test`, `examples`, documentation and the other platforms. Licence: zlib (`zlib-1.3.1\LICENSE`).

`dcmtk-3.7.0` is the same subset used by `C:\repos\I3Dfileformat` (copied from its `third_party` on
2026-10-23, identical byte for byte); see its `ORIGIN.txt` for where it comes from and what was
left out. Licence: BSD-style (`dcmtk-3.7.0\COPYRIGHT`), with the IJG and CharLS notices inside.
