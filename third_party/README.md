# third_party

Code of others, copied verbatim: do not edit. Only needed to build `bin\DicomShell.dll`
(`tools\Build.ps1`); using the tool needs nothing from here.

| Folder | What | Used for |
|---|---|---|
| `dcmtk-3.7.1` | DCMTK 3.7.1 (OFFIS), a subset of 8 modules: ofstd, oflog, oficonv (character sets, its tables built in), dcmdata, dcmimgle, dcmimage, dcmjpeg (with the IJG JPEG libraries) and dcmjpls (with its own CharLS) | the preview of DICOM files: decoding the pixel data (uncompressed, JPEG baseline / extended / lossless, JPEG-LS, RLE), rendering it, the names of the header attributes, and its text in any of the character sets of DICOM |
| `openjpeg-2.5.4` | OpenJPEG 2.5.4 (uclouvain), only its library `openjp2` and the CMake files it needs | decoding JPEG 2000 and High-Throughput JPEG 2000 pixel data, which DCMTK cannot |
| `libjxl-0.12.0` | libjxl 0.12.0 (the JPEG XL project), with the Highway, Brotli and skcms it builds with; only its decoder (`jxl_dec`) is built | decoding JPEG XL pixel data (lossless, recompressed JPEG, lossy), which DCMTK cannot |
| `zlib-1.3.1` | zlib 1.3.1 (Jean-loup Gailly and Mark Adler), its library and the CMake files it needs | DCMTK reading deflated datasets, and the deflated image frames DicomShell decodes itself |

`openjpeg-2.5.4` comes from the release tarball `https://github.com/uclouvain/openjpeg/archive/refs/tags/v2.5.4.tar.gz`
(SHA-256 `a695fbe19c0165f295a8531b1e4e855cd94d0875d2f88ec4b61080677e27188a`, downloaded 2026-10-27),
with only these parts of it, unchanged: the top-level files (`CMakeLists.txt`, `LICENSE`, `*.md`,
`CTestConfig.cmake`), `cmake\`, `src\CMakeLists.txt`, `src\lib\CMakeLists.txt`, `src\lib\openjp2\` and
`wrapping\CMakeLists.txt`. Left out: the command-line tools and their libraries (`src\bin`,
`thirdparty`), JPIP, Java, tests and documentation. Licence: BSD 2-clause (`openjpeg-2.5.4\LICENSE`).

`zlib-1.3.1` comes from the release tarball `https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz`
(SHA-256 `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`, the published one, downloaded
2026-10-26), with only these parts of it, unchanged: `CMakeLists.txt`, `LICENSE`, `README`, `ChangeLog`,
`zconf.h.cmakein`, `zlib.pc.cmakein`, `zlib.map`, the sources (`*.c`, `*.h` but `zconf.h`, which its CMake
makes from `zconf.h.cmakein` and would otherwise rename in this folder) and `win32\`. Left out: `contrib`,
`test`, `examples`, documentation and the other platforms. Licence: zlib (`zlib-1.3.1\LICENSE`).

`dcmtk-3.7.1` comes from the official repository, tag `DCMTK-3.7.1` (cloned 2026-10-27), with the same
subset as the 3.7.0 before it; see its `ORIGIN.txt` for the commit
and what was left out. Licence: BSD-style (`dcmtk-3.7.1\COPYRIGHT`), with the IJG and CharLS notices inside.

`libjxl-0.12.0` comes from the official repository, `git clone --depth 1 --branch v0.12.0
https://github.com/libjxl/libjxl` (commit `a7a9c787341cf703dede03c2009fa460cae5e5df`, cloned 2026-10-27,
without end-of-line conversion), with the submodules it pins for these three:
`third_party\highway` (google/highway, commit `457c891775a7397bdb0376bb1031e6e027af1c48`),
`third_party\brotli` (google/brotli, `028fb5a23661f123017c060daa546b55cf4bde29`) and `third_party\skcms`
(google/skcms, `96d9171c94b937a1b5f0293de7309ac16311b722`). Only these parts, unchanged: the top-level
`CMakeLists.txt`, `LICENSE`, `PATENTS`, `AUTHORS`, `CONTRIBUTORS`, `README.md`, `CHANGELOG.md`,
`cmake_uninstall.cmake.in`, `cmake\`, `lib\` and `tools\` (its CMake adds it, building nothing of it here);
of `third_party\`, `CMakeLists.txt`, `testing.cmake`, `skcms.cmake`, `lcms2.cmake`, `sjpeg.cmake` and
`dirent.*`; of Highway its CMake files, `LICENSE*`, `README.md`, `*.pc.in` and `hwy\`; of Brotli
`CMakeLists.txt`, `LICENSE`, `README.md`, `c\` and `scripts\`; of skcms `LICENSE`, `README.md`,
`skcms.h`, `skcms.cc` and `src\`. Left out: the other submodules (lcms, libpng, zlib, sjpeg,
libjpeg-turbo, googletest, testdata), the documentation, examples, plugins and the other languages of
Brotli. Licences: BSD 3-clause (`libjxl-0.12.0\LICENSE`, with its `PATENTS` grant), Apache 2.0 or
BSD 3-clause (Highway), MIT (Brotli), BSD 3-clause (skcms).
