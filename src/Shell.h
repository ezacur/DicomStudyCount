// What the parts of DicomShell (bin\DicomShell.dll) share: the libraries, and the headers of each part
#pragma once

// DCMTK first: its configuration must come before the Windows headers it includes itself
#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/dcistrmb.h"
#include "dcmtk/dcmdata/dcpxitem.h"
#include "dcmtk/dcmdata/dcrledrg.h"
#include "dcmtk/dcmimgle/dcmimage.h"
#include "dcmtk/dcmimgle/dipixel.h"
#include "dcmtk/dcmimage/diregist.h"
#include "dcmtk/dcmjpeg/djdecode.h"
#include "dcmtk/dcmjpls/djdecode.h"
#include "dcmtk/oflog/oflog.h"
#include "openjpeg.h"
#include "zlib.h"
#include <memory>
#include <mutex>

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <thumbcache.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <new>
#include <numeric>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "PlaneClouds.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "normaliz.lib")
#pragma comment(lib, "msimg32.lib")

// {F4798986-2EEA-49F6-8082-2FC552CE691B}
static const CLSID CLSID_DicomIcons = { 0xF4798986, 0x2EEA, 0x49F6, { 0x80, 0x82, 0x2F, 0xC5, 0x52, 0xCE, 0x69, 0x1B } };

// {21329A2B-09B3-4250-9103-9267877321EB}
static const CLSID CLSID_DicomProperties = { 0x21329A2B, 0x09B3, 0x4250, { 0x91, 0x03, 0x92, 0x67, 0x87, 0x73, 0x21, 0xEB } };

// {F3E184D5-95DF-4BE9-9929-32FF8A59CD55}
static const CLSID CLSID_DicomPreview = { 0xF3E184D5, 0x95DF, 0x4BE9, { 0x99, 0x29, 0x32, 0xFF, 0x8A, 0x59, 0xCD, 0x55 } };

// {F6D7D002-C00D-4893-AC73-DC78D44316C3}
static const CLSID CLSID_DicomThumbnails = { 0xF6D7D002, 0xC00D, 0x4893, { 0xAC, 0x73, 0xDC, 0x78, 0xD4, 0x43, 0x16, 0xC3 } };

extern HMODULE g_module;

extern LONG g_locks;

// Icon cache key: changes when the drawing changes, so Explorer never mixes old and new icons (the name
// of the suite before in it: Explorer keeps these keys in its icon cache)
static const wchar_t KeyPrefix[] = L"DicomStudyCount|v2|";

#include "HeaderReader.h"
#include "Icons.h"
#include "Properties.h"
#include "Decode.h"
#include "Segments.h"
#include "Preview.h"
#include "Thumbnails.h"
#include "Gather.h"
#include "Pixels.h"
#include "Export.h"
#include "Navigator.h"
#include "Plot.h"
#include "Explorer.h"
