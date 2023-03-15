/*******************************************************************************
 *
 * Copyright (C) 2022 NETINT Technologies
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 ******************************************************************************/

/*!*****************************************************************************
 *  \file   ni_rsrc_namespace.c
 *
 *  \brief  This utility aims to set the NVMe namespace number for a Quadra NVMe
 *          block device. It can operate on physical devices (PCIe physical
 *          function) or virtual devices (PCIe virtual function). Before setting
 *          namespace number, use SR-IOV to create the PCIe virtual function.
 *          Note that only block device name is accepted for this utility.
 *
 *          To effect the name space change, reload the NVMe driver:
 *              sudo modprobe -r nvme
 *              sudo modprobe nvme
 *              sudo nvme list  #check the result with nvme list
 ******************************************************************************/

#include "ni_device_api.h"
#include "ni_nvme.h"
#ifdef _WIN32
#include "ni_getopt.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define NI_NAMESPACE_SZ           32
#define NI_NAMESPACE_MAX_NUM      64

static void usage(void)
{
    printf("usage: ni_rsrc_namespace [OPTION]\n"
           "Provides NETINT QUADRA NVMe block device namespace IO operations.\n"
           "  -v    show version.\n"
           "  -d    the nvme block namespace.\n"
           "  -n    the nvme block namespace count.\n"
           "        Default: 0\n"
           "  -s    index of virtual PCIe functions in SR-IOV tied to the \n"
           "        physical PCIe function. '0' to select physical PCIe \n"
           "        function.\n"
           "        Eg. '1' to select the first virtual SR-IOV device tied \n"
           "        to the physical block device defined by '-d' option.\n"
           "        Default: 0\n"
           "  -h    help info.\n");
}

int main(int argc, char *argv[])
{
    ni_device_handle_t handle;
    ni_retcode_t retval;
    int opt;
    char device_namespace[NI_NAMESPACE_SZ] = {'\0'};
    int namespace_num = 1;
    int sriov_index = 0;
#ifdef __linux__
    struct stat sb;
#endif

    while ((opt = getopt(argc, argv, "d:n:s:hv")) != EOF)
    {
        switch (opt)
        {
            case 'h':
                usage();
                exit(0);
            case 'v':
                printf("Release ver: %s\n"
                       "API ver:     %s\n"
                       "Date:        %s\n"
                       "ID:          %s\n",
                       NI_XCODER_REVISION, LIBXCODER_API_VERSION,
                       NI_SW_RELEASE_TIME, NI_SW_RELEASE_ID);
                exit(0);
            case 'd':
                strcpy(device_namespace, optarg);
#ifdef __linux__
                if (lstat(device_namespace, &sb) != 0 ||
                    (sb.st_mode & S_IFMT) != S_IFBLK)
                {
                    fprintf(stderr, "ERROR: Only block device is supported! "
                            "%s is not block device!\n", device_namespace);
                    exit(-1);
                }
#endif
                break;
            case 'n':
                // A maximum of 64 namespaces are supported for firmware
                namespace_num = atoi(optarg);
                if (namespace_num < 0 || namespace_num > NI_NAMESPACE_MAX_NUM)
                {
                    fprintf(stderr, "ERROR: The number of namespace cannot "
                            "exceed %d\n", NI_NAMESPACE_MAX_NUM);
                    exit(-1);
                }
                break;
            case 's':
                sriov_index = atoi(optarg);
                if (sriov_index < 0)
                {
                    fprintf(stderr, "ERROR: Invalid SR-IOV device index: %d\n",
                            sriov_index);
                    exit(-1);
                }
                break;
            default:
                fprintf(stderr, "ERROR: Invalid option: %c\n", opt);
                exit(-1);
        }
    }

    if (device_namespace[0] == '\0')
    {
        fprintf(stderr, "ERROR: missing argument for -d\n");
        exit(-1);
    }

    handle = ni_device_open(device_namespace, NULL);
    if (handle == NI_INVALID_DEVICE_HANDLE)
    {
        fprintf(stderr, "ERROR: open %s failure for %s\n", device_namespace,
                strerror(NI_ERRNO));
        exit(-1);
    }
    else
    {
        printf("Succeed to open block namespace %s\n", device_namespace);
    }

    retval = ni_device_config_namespace_num(handle, namespace_num, sriov_index);
    if (retval != NI_RETCODE_SUCCESS)
    {
        fprintf(stderr, "ERROR: Namespace setting failure for %s\n",
                strerror(NI_ERRNO));
    }
    else
    {
        printf("Namespace setting succeed with number of %d and SR-IOV "
               "index %d\n", namespace_num, sriov_index);
    }

    ni_device_close(handle);

    return 0;
}
