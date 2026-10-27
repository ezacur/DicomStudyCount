/*
 *
 *  Copyright (C) 2026, Open Connections GmbH
 *  All rights reserved.  See COPYRIGHT file for details.
 *
 *  This software and supporting documentation were developed by
 *
 *    Open Connections GmbH
 *    Escherweg 2
 *    D-26121 Oldenburg, Germany
 *
 *
 *  Module:  dcmdata
 *
 *  Author:  Michael Onken
 *
 *  Purpose: test program for class DicomDirInterface
 *
 */

#include "dcmtk/config/osconfig.h"    /* make sure OS specific configuration is included first */

#include "dcmtk/ofstd/oftest.h"
#include "dcmtk/dcmdata/dcddirif.h"


// Validation of Referenced File IDs read from a (possibly malicious) DICOMDIR
// by DicomDirInterface::isReferencedFileIDSafe(). Only a conformant DICOM File ID
// (backslash-separated, non-empty components of uppercase letters, digits and
// underscore) may be accepted; everything that could escape the DICOMDIR
// directory (path traversal) must be rejected.
OFTEST(dcmdata_referencedFileIDSafety)
{
    // conformant, relative File IDs -- these must be accepted
    const char *validIDs[] =
    {
        "IMG00001",                 // single component
        "IMG001\\IMG00001",         // two components
        "DICOM\\IMAGES\\IM000001",  // three components
        "A\\B\\C\\D\\E\\F\\G\\H",    // eight single-character components
        "REPORT_1\\SCAN_02",        // underscores and digits
        "0",                        // a single digit
        "PATIENT_1"                 // underscore inside a component
    };
    // unsafe or malformed values -- these must be rejected
    const char *invalidIDs[] =
    {
        "",                         // empty value
        ".",                        // current directory
        "..",                       // parent reference
        "..\\OUTDIR\\SECRET",       // leading parent reference (path traversal)
        "FOO\\..\\BAR",             // embedded parent reference
        "FOO\\..",                  // trailing parent reference
        ".\\FOO",                   // leading current directory
        "....",                     // more than two periods
        "FOO..",                    // periods at the end of a component
        "\\",                       // separator only
        "\\ABS\\SECRET",            // leading backslash -> absolute path
        "FOO\\BAR\\",               // trailing backslash -> empty last component
        "FOO\\\\BAR",               // double backslash -> empty component
        "/FOO",                     // leading slash -> absolute path
        "/etc/passwd",              // POSIX absolute path
        "C:\\WINDOWS\\SYSTEM32",    // Windows drive-letter path
        "C:FOO",                    // Windows drive-relative path
        "FILE.DCM",                 // '.' is not part of the File ID character set
        "FOO/BAR",                  // '/' is not a valid separator here
        "FOO BAR",                  // space is not allowed
        "FILE-01",                  // '-' is not part of the File ID character set
        "FILE;1",                   // ISO 9660 version suffix
        "~",                        // home directory shortcut
        "FOO\\B*R"                  // wildcard
    };
    // not conformant because of the lowercase letters, but just as safe. These
    // are only accepted if the caller passes allowLowercase = OFTrue
    const char *lowercaseIDs[] =
    {
        "img001",                   // all lowercase
        "Img001",                   // mixed case
        "dir\\img001",              // lowercase in more than one component
        "report_1\\scan_02"         // lowercase with underscores and digits
    };
    size_t i;
    for (i = 0; i < sizeof(validIDs) / sizeof(validIDs[0]); ++i)
    {
        // a conformant File ID is accepted regardless of the parameter
        if (!DicomDirInterface::isReferencedFileIDSafe(validIDs[i]))
            OFCHECK_FAIL("valid Referenced File ID rejected: \"" << validIDs[i] << "\"");
        if (!DicomDirInterface::isReferencedFileIDSafe(validIDs[i], OFTrue /*allowLowercase*/))
            OFCHECK_FAIL("valid Referenced File ID rejected with allowLowercase: \"" << validIDs[i] << "\"");
    }
    for (i = 0; i < sizeof(invalidIDs) / sizeof(invalidIDs[0]); ++i)
    {
        // allowLowercase must not accept anything but lowercase letters, so an
        // unsafe value has to be rejected in both cases
        if (DicomDirInterface::isReferencedFileIDSafe(invalidIDs[i]))
            OFCHECK_FAIL("unsafe Referenced File ID accepted: \"" << invalidIDs[i] << "\"");
        if (DicomDirInterface::isReferencedFileIDSafe(invalidIDs[i], OFTrue /*allowLowercase*/))
            OFCHECK_FAIL("unsafe Referenced File ID accepted with allowLowercase: \"" << invalidIDs[i] << "\"");
    }
    for (i = 0; i < sizeof(lowercaseIDs) / sizeof(lowercaseIDs[0]); ++i)
    {
        if (DicomDirInterface::isReferencedFileIDSafe(lowercaseIDs[i]))
            OFCHECK_FAIL("non-conformant Referenced File ID accepted by default: \"" << lowercaseIDs[i] << "\"");
        if (!DicomDirInterface::isReferencedFileIDSafe(lowercaseIDs[i], OFTrue /*allowLowercase*/))
            OFCHECK_FAIL("Referenced File ID rejected with allowLowercase: \"" << lowercaseIDs[i] << "\"");
    }
}
