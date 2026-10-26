// The quick reading of a DICOM header (up to the attributes asked), without DCMTK: for the icons and the columns
// (included by Shell.h, after the headers of the parts before it)
#pragma once

enum class Kind { NotDicom, Dicom };

struct Header
{
    std::string charset, studyUid, seriesNumber, seriesUid, seriesDescription, modality, sopUid;
    std::string patientId, instanceNumber, imagePosition, imageOrientation;
    unsigned rows = 0, columns = 0;                     // 0: not in the header
    std::string transferSyntax, sopClass, imageType, manufacturer, protocolName, frameOfReference;
    std::string acquisitionDateTime, acquisitionDate, acquisitionTime, contentDate, contentTime;
    std::string seriesDate, seriesTime, studyDate;
    std::string numberOfFrames, triggerTime, sliceThickness, pixelSpacing;
    std::string scanningSequence, sequenceName, inversionTime, cardiacImages;   // (what kind of series it is)
    // Identifying data, for the "sensitive data" column (its values are never shown)
    std::string patientName, birthDate, address, phone, otherNames, otherIds, motherName;
    std::string institution, institutionAddress, referring, performing, operators;
    std::string identityRemoved, burnedIn;
};

// Where the reading stops: the icons need the header up to SeriesNumber only, the columns up to
// BurnedInAnnotation (the last attribute they look at)
static const unsigned UpToSeriesNumber = 0x00200011;

static const unsigned UpToBurnedIn = 0x00280301;

static const char DirectorySopClass[] = "1.2.840.10008.1.3.10";   // DICOMDIR

Kind ReadHeader(const wchar_t* path, Header& h, unsigned last);
