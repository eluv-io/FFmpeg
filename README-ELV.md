## Eluvio fork of FFmpeg

### History

- 2018/03 https://github.com/FFmpeg/FFmpeg f09fdf2d9c0f5acc60c4572b6d7bb211f7a2aca0
- 2019/01 https://github.com/FFmpeg/FFmpeg ab160efa2850715de32e24fed846a0f6ef7244ab
  - Notable: LHLS support

## Building for Eluvio

https://trac.ffmpeg.org/wiki/CompilationGuide/Ubuntu

Linux:

./configure --enable-shared --enable-pthreads --enable-version3 --enable-hardcoded-tables --enable-avresample --cc=clang --host-cflags=-fPIC --host-ldflags= --extra-cflags=-fPIC --enable-gpl --enable-libmp3lame --enable-libx264 --enable-libxvid --enable-opencl --disable-lzma

Mac:

./configure --prefix=/usr/local/Cellar/ffmpeg/3.4.2 --enable-shared --enable-pthreads --enable-version3 --enable-hardcoded-tables --enable-avresample --cc=clang --host-cflags=-fPIC --host-ldflags= --disable-jack --enable-gpl --enable-libmp3lame --enable-libx264 --enable-libxvid --enable-opencl --enable-videotoolbox --disable-lzma


## Debugging

In `configure` file, insert this code at the end of the `probe_cc()` function:
```
_cflags_speed='-O0'
_cflags_size='-O0'
```

Use these `configure` flags:
```
--enable-debug=3 --disable-stripping --disable-optimizations
```
