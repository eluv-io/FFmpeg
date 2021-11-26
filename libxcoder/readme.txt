
Logan Xcoder Codec Library User Guide:



==============================
To configure and build library
==============================

---------------------------
To default install location:
---------------------------

cd libxcoder
./build.sh


Codec library 'libxcoder.a' is by default installed to         /usr/local/lib
Codec library 'libxcoder.so' is by default installed to        /usr/local/lib
Codec Pkg Config 'xcoder.pc' is by default installed to        /usr/local/lib/pkgconfig
Codec API Header files are by default installed to             /usr/local/include
Standalone test program 'xcoder' is locally generated in       libxcoder/build


--------------------------
To fully customize install:
--------------------------

./configure --libdir=/custom_lib_folder --bindir=/custom_bin_folder \
--includedir=/custom_include_folder --shareddir=/additional_lib_folder && sudo make install


------------
To uninstall:
------------

sudo make uninstall

or

sudo make uninstall LIBDIR=/custom_lib_folder BINDIR=/custom_bin_folder \
INCLUDEDIR=/custom_include_folder SHAREDDIR=/additional_lib_folder


-------------
Build Options:
-------------

To enable support for old NVMe drivers, e.g. CentOS 6.5 (default: --without-old-nvme-driver)
./configure --with-old-nvme-driver

To enable libxcoder self termination on repeated NVMe error
./configure --with-self-kill

To enable libxcoder latency test patch (default: --without-latency-patch)
./configure --with-latency-patch

To set custom installation path for libxcoder.so and pkgconfig files
./configure --libdir custom_lib_folder/

To set custom installation path for binary utilities (ni_rsrc_mon, etc.)
./configure --bindir custom_bin_folder/

To set custom installation path for libxcoder headers
./configure --includedir custom_include_folder/

To set additional installation path for libxcoder.so
./configure --shareddir additional_lib_folder/

==============================
To run standalone test program
==============================
Note: for now, decoding/encoding/transcoding has the following limitations:
 - 8 bit YUV420p only.
 - no audio/container support.
 - 1.8 GB (or less) input file size

------------
Test Decoder:
------------

 cd libxcoder/build
 sudo ./xcoder 0 ../test/1280x720p_Basketball.264 basketball-dec.yuv 1280 720 decode 0 131040 3

------------
Test Encoder:
------------

 cd libxcoder/build
 sudo ./xcoder 0  ../test/basketball-dec.yuv basketball-enc.265 1280 720 encode 1 131040 3
 
---------------
Test Transcoder:
---------------

 cd libxcoder/build
 sudo ./xcoder 0 ../test/1280x720p_Basketball.264 basketball-xcod.265 1280 720 xcode 0-1 131040 3



 
===================================
To Integrate into user applications
===================================

-------------------
FFmpeg applications:
-------------------

Configure and build FFmpeg with:

./configure --enable-libxcoder && make


------------------
Other applications:
------------------

Codec library: libxcoder.a
API header files: ni_device_api.h ni_rsrc_api.h

1. Add libxcoder.a as one of libraries to link
2. Add ni_device_api.h in source code calling Codec API

For C
#include "ni_device_api.h"

For C++

extern "C" {
#include "ni_device_api.h"
}
