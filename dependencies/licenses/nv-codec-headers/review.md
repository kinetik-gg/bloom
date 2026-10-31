# nv-codec-headers 12.1.14.1 intake review

## Role and boundary

nv-codec-headers is FFmpeg's copy of NVIDIA's Video Codec SDK interface headers
(`nvEncodeAPI.h`, the CUDA and NVDEC declarations, and FFmpeg's `dynlink_loader.h`). It is
header-only: the superbuild installs the unmodified headers and the `ffnvcodec` pkg-config record
into the dependency prefix (Linux and Windows; macOS has no NVENC), and FFmpeg's `h264_nvenc` and
`hevc_nvenc` encoders compile against them. At run time FFmpeg loads the NVIDIA driver's own
`libnvidia-encode`/`libcuda` (or `nvEncodeAPI64.dll`/`nvcuda.dll`) with `dlopen`/`LoadLibrary`;
nothing from NVIDIA is linked into or shipped with Bloom. A computer without the driver has no NVENC
provider and the encode chain falls through to VA-API or the bundled software encoders.

## Licence

Every header carries the MIT licence (`LICENSE`, copied from the headers' own notice; NVIDIA
Corporation and the FFmpeg contributors who wrote the dynamic loader). The compiled declarations end
up in the LGPL `libavcodec`, so the notice is reproduced in `THIRD_PARTY_NOTICES.md` and in Bloom's
licence catalogue. Enabling NVENC needs neither `--enable-nonfree` nor `--enable-gpl`: FFmpeg
classifies `ffnvcodec`/`nvenc` as LGPL-compatible. The NVIDIA driver is the user's own system
component, not a Bloom dependency.

## Version

The 12.1 line is the newest whose minimum driver (Linux 530.41.03, Windows 531.61) covers drivers
from 2023 on; newer header lines would refuse older drivers at run time. FFmpeg 8.1 accepts 12.1.14.0
and newer.

## Qualification posture

NVENC H.264 High and HEVC Main/Main 10 are hardware execution keys with no determinism claim, served
only after the worker opens an encode session on the device; every export still passes the reopen
check.
