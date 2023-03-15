#!/bin/bash

# This is to package all the bin files.
# ffmpeg.exe, ffplay.exe, ffprobe.exe and their releated dll.

XCODER_WORKDIR="."
XCODER_BIN_DIR="bin"
XCODER_BIN="${XCODER_WORKDIR}/${XCODER_BIN_DIR}"
XCODER_LOCAL_PATH="/usr/local"

PLATFORM=$(uname -s)
echo ${PLATFORM}

if [[ $PLATFORM =~ "MINGW32" ]]; then
    MIGNW_PATH="/mingw32/bin"
else
    MIGNW_PATH="/mingw64/bin"
fi

# Create bin folder for compilation
if [ ! -d "${XCODER_WORKDIR}/${XCODER_BIN_DIR}" ]; then
    mkdir ${XCODER_BIN}
fi

# Remove the old files
rm -rf  ${XCODER_BIN}/*

echo "Package the exe and libraries to folder ${XCODER_BIN}"

cp -v ${XCODER_WORKDIR}/ffmpeg.exe ${XCODER_BIN}

if $enable_ffplay; then
    cp -v ${XCODER_WORKDIR}/ffplay.exe ${XCODER_BIN}
fi

if $enable_ffprobe; then
    cp -v ${XCODER_WORKDIR}/ffprobe.exe ${XCODER_BIN}
fi

if $enable_x264; then
    cp -v ${MIGNW_PATH}/libx264-161.dll ${XCODER_BIN}
fi

if $enable_x265; then
    cp -v ${MIGNW_PATH}/libx265.dll ${XCODER_BIN}
    cp -v ${MIGNW_PATH}/libstdc++-6.dll ${XCODER_BIN}
    cp -v ${MIGNW_PATH}/libgcc_s_seh-1.dll ${XCODER_BIN}
fi

if $enable_ffnvcodec; then
    echo "Warning: don't support to package nvcodec now!!"
    exit 1
fi

if $enable_vmaf; then
    echo "Warning: don't support to package vmaf now!!"
    exit 1
fi

cp -v ${MIGNW_PATH}/libbz2-1.dll ${MIGNW_PATH}/libiconv-2.dll ${MIGNW_PATH}/liblzma-5.dll ${MIGNW_PATH}/libwinpthread-1.dll ${MIGNW_PATH}/zlib1.dll ${MIGNW_PATH}/SDL2.dll ${XCODER_BIN}

if [[ $PLATFORM =~ "MINGW32" ]]; then
    cp -v ${MIGNW_PATH}/libgcc_s_dw2-1.dll ${XCODER_BIN}
fi

if $enable_shared; then
    SUBFIX="dll"
    cp -v ${XCODER_WORKDIR}/libavcodec/*avcodec-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavdevice/*avdevice-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavfilter/*avfilter-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavformat/*avformat-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavutil/*avutil-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libpostproc/*postproc-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libswresample/*swresample-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libswscale/*swscale-*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_LOCAL_PATH}/bin/libxcoder.${SUBFIX} ${XCODER_BIN}

    SUBFIX="lib"
    cp -v ${XCODER_WORKDIR}/libavcodec/*avcodec*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavdevice/*avdevice*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavfilter/*avfilter*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavformat/*avformat*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavutil/*avutil*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libpostproc/*postproc*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libswresample/*swresample*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libswscale/*swscale*.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_LOCAL_PATH}/bin/libxcoder.${SUBFIX} ${XCODER_BIN}
else
    SUBFIX="a"
    cp -v ${XCODER_WORKDIR}/libavcodec/*avcodec.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavdevice/*avdevice.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavfilter/*avfilter.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavformat/*avformat.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libavutil/*avutil.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libpostproc/*postproc.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libswresample/*swresample.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_WORKDIR}/libswscale/*swscale.${SUBFIX} ${XCODER_BIN}
    cp -v ${XCODER_LOCAL_PATH}/lib/libxcoder.${SUBFIX} ${XCODER_BIN}
fi

exit 0
