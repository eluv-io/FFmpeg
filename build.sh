PREFIX="dist"

if [ ! -d $PREFIX ]
then
    mkdir $PREFIX
    echo "CREATED" $PREFIX
fi

CONFIGURE_TO_USE="./configure"

if [ "$(uname)" == "Darwin" ]; then
    if [ "$1" == "--DEBUG" ] || [ "$1" == "--debug" ]; then
        command_options="--prefix=${PREFIX} --enable-libfreetype --enable-libfribidi --enable-libfontconfig --enable-shared --enable-avresample --enable-pthreads --enable-version3 --enable-hardcoded-tables --cc=clang --host-cflags=-fPIC --host-ldflags=  --enable-gpl --enable-libmp3lame --enable-libx264 --enable-libx265 --enable-libxvid --enable-opencl --enable-videotoolbox --disable-lzma --disable-stripping --enable-debug=3 --extra-cflags=-ggdb --extra-cflags=-O0 --extra-cflags=-g"
        CONFIGURE_TO_USE="./configure.debug"
    else
        command_options="--prefix=${PREFIX} --enable-libfreetype --enable-libfribidi --enable-libfontconfig --enable-shared --enable-avresample --enable-pthreads --enable-version3 --enable-hardcoded-tables --cc=clang --host-cflags=-fPIC --host-ldflags=  --enable-gpl --enable-libmp3lame --enable-libx264 --enable-libx265 --enable-libxvid --enable-opencl --enable-videotoolbox --disable-lzma"
    fi
    echo "configuring with command_options=${command_options}"
    ${CONFIGURE_TO_USE} ${command_options}
elif [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
    if [ "$1" == "--DEBUG" ] || [ "$1" == "--debug" ]; then
        command_options="--prefix=${PREFIX} --enable-libfreetype --enable-libfribidi --enable-libfontconfig --enable-shared --enable-avresample --enable-pthreads --enable-version3 --enable-hardcoded-tables --cc=clang --host-cflags=-fPIC --host-ldflags= --extra-cflags=-fPIC --enable-gpl --enable-libmp3lame --enable-libx264 --enable-libx265 --enable-libxvid --enable-opencl --disable-lzma --disable-stripping --enable-debug=3 --extra-cflags=-ggdb --extra-cflags=-O0 --extra-cflags=-g"
        CONFIGURE_TO_USE="./configure.debug"
    else
        command_options="--prefix=${PREFIX} --enable-libfreetype --enable-libfribidi --enable-libfontconfig --enable-shared --enable-avresample --enable-pthreads --enable-version3 --enable-hardcoded-tables --cc=clang --host-cflags=-fPIC --host-ldflags= --extra-cflags=-fPIC --enable-gpl --enable-libmp3lame --enable-libx264 --enable-libx265 --enable-libxvid --enable-opencl --disable-lzma"
    fi
    #FLAGS_NVIDIA="--enable-cuda --enable-cuvid --enable-nvenc --enable-nonfree --enable-libnpp --extra-cflags=-I/usr/local/cuda/include --extra-ldflags=-L/usr/local/cuda/lib64"
    echo "configuring with command_options=${FLAGS_NVIDIA} ${command_options}"
    ${CONFIGURE_TO_USE} ${FLAGS_NVIDIA} ${command_options}
elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW32_NT" ]; then
    echo "MINGW32 Not yet supported"
elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW64_NT" ]; then
    echo "MINGW64 Not yet supported"
fi

NPROCS=$(getconf _NPROCESSORS_ONLN)
ELUVIO_BUILD_THREAD_COUNT=${ELUVIO_BUILD_THREAD_COUNT:-"-j${NPROCS}"}
ELUVIO_TEST_THREAD_COUNT=${ELUVIO_TEST_THREAD_COUNT:-"${NPROCS}"}
ELUVIO_BUILD_THREAD_COUNT=${ELUVIO_BUILD_THREAD_COUNT:-"-j4"}

make ${ELUVIO_BUILD_THREAD_COUNT} && make install
