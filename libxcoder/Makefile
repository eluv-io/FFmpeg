.PHONY: all default test clean cleanall install uninstall
WINDOWS ?= FALSE
GDB ?= FALSE
SHARED ?= FALSE

INSTALL = install
LIBXCODER_CUSTOM_PARAMETERS ?=

ifeq ($(GDB), TRUE)
	OPTFLAG = -g
else
	OPTFLAG = -O3
endif

PLATFORM = $(shell uname -s)
ifeq ($(findstring MSYS,$(PLATFORM)), MSYS)
	HOST_OS = MINGW
	SUBFIX = .exe
	ARCH = 64
	DLL_MACHINE = i386:x86-64
else ifeq ($(findstring MINGW,$(PLATFORM)), MINGW)
	HOST_OS = MINGW
	SUBFIX = .exe
ifeq ($(findstring MINGW64,$(PLATFORM)), MINGW64)
	ARCH = 64
	DLL_MACHINE = i386:x86-64
else
	ARCH = 32
	DLL_MACHINE = i386
endif
else
	HOST_OS = Linux
	SUBFIX =
endif

CFLAGS = -fPIC -DLIBXCODER_OBJS_BUILD
CPPFLAGS = -D__STDC_FORMAT_MACROS
TARGETNAME = xcoder
TARGET = ${TARGETNAME}
TARGET_LIB = lib${TARGETNAME}.a
ifeq ($(HOST_OS), Linux)
TARGET_LIB_SHARED = lib${TARGETNAME}.so
TARGET_VERSION = $(shell grep 'Version: '.* < bin/xcoder.pc  | cut -d ' ' -f 2)
else
TARGET_LIB_SHARED = lib${TARGETNAME}.dll
endif
TARGET_INC = ni_device_api.h
TARGET_INC2 = ni_rsrc_api.h
TARGET_INC3 = ni_defs.h
TARGET_INC4 = ni_util.h
TARGET_INC5 = ni_log.h
TARGET_PC = xcoder.pc
OBJECTS =  ni_nvme.o ni_device_api_priv.o ni_device_api.o ni_util.o ni_log.o ni_rsrc_priv.o ni_rsrc_api.o
LINK_OBJECTS =  ${OBJS_PATH}/ni_nvme.o ${OBJS_PATH}/ni_device_api_priv.o ${OBJS_PATH}/ni_device_api.o ${OBJS_PATH}/ni_util.o ${OBJS_PATH}/ni_log.o ${OBJS_PATH}/ni_rsrc_priv.o ${OBJS_PATH}/ni_rsrc_api.o
ALL_OBJECTS = ni_device_test.o init_rsrc.o test_rsrc_api.o ni_rsrc_mon.o ni_rsrc_update.o ni_rsrc_list.o ${OBJECTS}
SRC_PATH = ./source
OBJS_PATH = ./build
BIN_PATH = ./bin

ifeq ($(CUSTOM_DRIVER), TRUE)
	CFLAGS += -DXCODER_LINUX_CUSTOM_DRIVER
endif

# Read the installation directory from path set in build/xcoder.pc
DESTDIR ?= $(shell sed -n 's/^prefix=\(.*\)/\1/p' $(BIN_PATH)/$(TARGET_PC))
LIBDIR = $(shell sed -n 's/^libdir=\(.*\)/\1/p' $(BIN_PATH)/$(TARGET_PC))
BINDIR = $(shell sed -n 's/^bindir=\(.*\)/\1/p' $(BIN_PATH)/$(TARGET_PC))
INCLUDEDIR = $(shell sed -n 's/^includedir=\(.*\)/\1/p' $(BIN_PATH)/$(TARGET_PC))
SHAREDDIR = $(shell sed -n 's/^shareddir=\(.*\)/\1/p' $(BIN_PATH)/$(TARGET_PC))
ifeq ($(HOST_OS), Linux)
	INCLUDES= -pthread -lrt ${OPTFLAG} -lstdc++
else
	#CC      = x86_64-w64-mingw32-gcc
	DLLTOOL = dlltool
	INCLUDES= ${OPTFLAG}
	#TARGET_INC_WIN1 = nvme.h 
	#TARGET_INC_WIN2 = nvmeIoctl.h
	#TARGET_INC_WIN3 = nvmeReg.h
	#TARGET_INC_WIN4 = ni_util.h
	#TARGET_INC_WIN5 = ni_log.h
endif

# make target

all:${ALL_OBJECTS}
ifeq ($(HOST_OS), Linux)
	ar rcs $(BIN_PATH)/${TARGET_LIB} ${LINK_OBJECTS}
ifeq ($(SHARED), TRUE)
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -shared -Wl,-soname,${TARGET_LIB_SHARED}.${TARGET_VERSION} -o $(BIN_PATH)/${TARGET_LIB_SHARED}.${TARGET_VERSION} $(LINK_OBJECTS) ${INCLUDES}
endif

	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(BIN_PATH)/${TARGET} $(OBJS_PATH)/ni_device_test.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(BIN_PATH)/test_rsrc_api $(OBJS_PATH)/test_rsrc_api.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(BIN_PATH)/ni_rsrc_mon $(OBJS_PATH)/ni_rsrc_mon.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(BIN_PATH)/ni_rsrc_update $(OBJS_PATH)/ni_rsrc_update.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(BIN_PATH)/ni_rsrc_list $(OBJS_PATH)/ni_rsrc_list.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(BIN_PATH)/init_rsrc $(OBJS_PATH)/init_rsrc.o $(LINK_OBJECTS) ${INCLUDES}

else
ifeq ($(SHARED), TRUE)
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -shared -o $(BIN_PATH)/${TARGET_LIB_SHARED} -Wl,--output-def,$(BIN_PATH)/$(TARGETNAME).def,--out-implib,$(BIN_PATH)/${TARGET_LIB_SHARED}.a $(LINK_OBJECTS) ${INCLUDES}
	${DLLTOOL} -m $(DLL_MACHINE) -D $(BIN_PATH)/${TARGET_LIB_SHARED} -d $(BIN_PATH)/$(TARGETNAME).def -l $(BIN_PATH)/lib${TARGETNAME}.lib
else
	ar rcs $(BIN_PATH)/${TARGET_LIB} ${LINK_OBJECTS}
endif

	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -static -o $(BIN_PATH)/${TARGET} $(OBJS_PATH)/ni_device_test.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -static -o $(BIN_PATH)/test_rsrc_api $(OBJS_PATH)/test_rsrc_api.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -static -o $(BIN_PATH)/ni_rsrc_mon $(OBJS_PATH)/ni_rsrc_mon.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -static -o $(BIN_PATH)/ni_rsrc_update $(OBJS_PATH)/ni_rsrc_update.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -static -o $(BIN_PATH)/ni_rsrc_list $(OBJS_PATH)/ni_rsrc_list.o $(LINK_OBJECTS) ${INCLUDES}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -static -o $(BIN_PATH)/init_rsrc $(OBJS_PATH)/init_rsrc.o $(LINK_OBJECTS) ${INCLUDES}

endif

ifeq ($(SHARED), TRUE)
	@echo info ${TARGET_LIB_SHARED}
endif
	@echo $(PLATFORM)

test:${ALL_OBJECTS}
	${CC} ${LIBXCODER_CUSTOM_PARAMETERS}  -o $(OBJS_PATH)/${TARGET} $(OBJS_PATH)/*.o ${INCLUDES}
	
install:
	mkdir -p ${INCLUDEDIR}/
	mkdir -p ${LIBDIR}/
	mkdir -p ${LIBDIR}/pkgconfig/
	mkdir -p ${BINDIR}/
	${INSTALL} -m 644 ${SRC_PATH}/${TARGET_INC} ${INCLUDEDIR}/.
	${INSTALL} -m 644 ${SRC_PATH}/${TARGET_INC2} ${INCLUDEDIR}/.
	${INSTALL} -m 644 ${SRC_PATH}/${TARGET_INC3} ${INCLUDEDIR}/.
	${INSTALL} -m 644 ${SRC_PATH}/${TARGET_INC4} ${INCLUDEDIR}/.
	${INSTALL} -m 644 ${SRC_PATH}/${TARGET_INC5} ${INCLUDEDIR}/.

ifeq ($(HOST_OS), Linux)
	${INSTALL} -m 755 ${BIN_PATH}/${TARGET_LIB} ${LIBDIR}/.
ifeq ($(SHARED), TRUE)
	${INSTALL} -m 755 ${BIN_PATH}/${TARGET_LIB_SHARED}.${TARGET_VERSION} ${LIBDIR}/.
	(cd ${LIBDIR}; ln -f -s ${TARGET_LIB_SHARED}.${TARGET_VERSION} ${TARGET_LIB_SHARED}; cd -)
ifneq ($(SHAREDDIR),)
	mkdir -p ${SHAREDDIR}/
	${INSTALL} -m 644 ${BIN_PATH}/${TARGET_LIB_SHARED}.${TARGET_VERSION} ${SHAREDDIR}/.
	(cd ${SHAREDDIR}; ln -f -s ${TARGET_LIB_SHARED}.${TARGET_VERSION} ${TARGET_LIB_SHARED}; cd -)
endif
endif
else
	mkdir -p ${BIN_PATH}/include/
	cp ${SRC_PATH}/${TARGET_INC} ${BIN_PATH}/include/.
	cp ${SRC_PATH}/${TARGET_INC2} ${BIN_PATH}/include/.
	cp ${SRC_PATH}/${TARGET_INC3} ${BIN_PATH}/include/.
	cp ${SRC_PATH}/${TARGET_INC4} ${BIN_PATH}/include/.
	cp ${SRC_PATH}/${TARGET_INC5} ${BIN_PATH}/include/.

ifeq ($(SHARED), TRUE)
	${INSTALL} -m 755 ${BIN_PATH}/${TARGET_LIB_SHARED} ${BINDIR}/.
	${INSTALL} -m 755 $(BIN_PATH)/lib${TARGETNAME}.lib ${BINDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/${TARGET_LIB_SHARED}.a ${LIBDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/$(TARGETNAME).def ${LIBDIR}/.
ifneq ($(SHAREDDIR),)
	mkdir -p ${SHAREDDIR}/
	${INSTALL} -m 644 ${BIN_PATH}/${TARGET_LIB_SHARED} ${SHAREDDIR}/.
	${INSTALL} -m 755 $(BIN_PATH)/lib${TARGETNAME}.lib ${SHAREDDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/${TARGET_LIB_SHARED}.a ${SHAREDDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/$(TARGETNAME).def ${SHAREDDIR}/.
endif
else
	${INSTALL} -m 755 ${BIN_PATH}/${TARGET_LIB} ${LIBDIR}/.
endif
endif

	${INSTALL} -m 644 ${BIN_PATH}/${TARGET_PC} ${LIBDIR}/pkgconfig/.
	${INSTALL} -m 755 ${BIN_PATH}/ni_rsrc_mon$(SUBFIX) ${BINDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/ni_rsrc_update$(SUBFIX) ${BINDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/ni_rsrc_list$(SUBFIX) ${BINDIR}/.
	${INSTALL} -m 755 ${BIN_PATH}/init_rsrc$(SUBFIX) ${BINDIR}/.

uninstall:
	rm -f ${INCLUDEDIR}/${TARGET_INC} ${INCLUDEDIR}/${TARGET_INC2} ${INCLUDEDIR}/${TARGET_INC3} ${INCLUDEDIR}/${TARGET_INC4} ${INCLUDEDIR}/${TARGET_INC5} ${LIBDIR}/${TARGET_LIB} ${LIBDIR}/${TARGET_LIB_SHARED} ${LIBDIR}/${TARGET_LIB_SHARED}.${TARGET_VERSION} ${LIBDIR}/${TARGET_LIB_SHARED}.a ${LIBDIR}/pkgconfig/${TARGET_PC} ${LIBDIR}/$(TARGETNAME).def ${BINDIR}/${TARGET_LIB_SHARED}  ${BINDIR}/lib${TARGETNAME}.lib ${BINDIR}/ni_rsrc_mon$(SUBFIX) ${BINDIR}/ni_rsrc_update$(SUBFIX) ${BINDIR}/ni_rsrc_list$(SUBFIX) ${BINDIR}/init_rsrc$(SUBFIX)
ifneq ($(SHAREDDIR),)
	rm -f ${SHAREDDIR}/${TARGET_LIB_SHARED}.${TARGET_VERSION} ${SHAREDDIR}/${TARGET_LIB_SHARED} 
endif

cleanall:clean

clean:
	rm -rf $(BIN_PATH)/*${TARGET}* $(OBJS_PATH)/*.o $(OBJS_PATH)/*.h $(BIN_PATH)/*$(SUBFIX) $(BIN_PATH)/include $(OBJS_PATH)/*.gcno $(OBJS_PATH)/*.gcda

# dependence
%.o : ${SRC_PATH}/%.cpp
	${CC} ${CFLAGS} ${LIBXCODER_CUSTOM_PARAMETERS} ${CPPFLAGS} ${INCLUDES} -c $< -o ${OBJS_PATH}/$@

%.o : ${SRC_PATH}/%.c
	${CC} ${CFLAGS} ${LIBXCODER_CUSTOM_PARAMETERS} ${INCLUDES} -c $< -o ${OBJS_PATH}/$@
