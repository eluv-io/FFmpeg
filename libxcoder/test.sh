#!/usr/bin/env bash

rm xcoder.log
cd ./build

sudo ./xcoder /dev/nvme0 ../test/test_720p.264 output.yuv 1280 720 decode > xcoder.log 2>&1
#sudo ./xcoder /dev/nvme0 ../test/akiyo_cif.yuv output.265 352 288 encode > xcoder.log 2>&1
#sudo ./xcoder /dev/nvme0 ../test/test_720p.264 output.265 1280 720 transcode > xcoder.log 2>&1
