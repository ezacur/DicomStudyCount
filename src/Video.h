// Video (MPEG-2, MPEG-4 AVC/H.264, HEVC/H.265): its frames by the decoders of Windows (Media Foundation)
// (included by Shell.h, after the headers of the parts before it)
#pragma once

bool IsVideo(E_TransferSyntax x);

// The video of a file, decoded frame after frame (VideoFrames: in Video.cpp); null if it cannot, and why
std::shared_ptr<VideoFrames> OpenVideo(DcmDataset* ds, std::wstring& why);

// The same for the explorer, which opens a file again for each frame: the one of the last file asked for
std::shared_ptr<VideoFrames> VideoOfFile(const std::wstring& path, DcmDataset* ds, std::wstring& why);

// Frame k as a copy of the dataset with it as native RGB pixels; null if it cannot, and why
DcmDataset* DecodeVideoFrame(VideoFrames& v, DcmDataset* ds, int frame, std::wstring& why);

// How many frames it has (0 if it cannot tell) and the time from one to the next (ms; left if not known)
int VideoFrameCount(VideoFrames& v, double& frameTime);
