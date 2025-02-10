cmake_minimum_required (VERSION 2.8.3)

project(ox C)

add_definitions (-DCONFIG_VERSION=2)
add_definitions (-DCONFIG_PATCHLEVEL=7)
add_definitions (-DCONFIG_SUBLEVEL=0)
add_definitions (-DCONFIG_LABEL="The Humble Bee")

set(CMAKE_C_FLAGS "-fPIC -g -pthread -fopenmp -Wall")

include_directories(${PROJECT_SOURCE_DIR}/include
                    ${PROJECT_SOURCE_DIR}/host
                    ${PROJECT_SOURCE_DIR}/transport/ox-fabrics
                    ${PROJECT_SOURCE_DIR}/ftl/lnvm
                    ${PROJECT_SOURCE_DIR}/ftl/ox-app
                    ${PROJECT_SOURCE_DIR}/mmgr/volt
                    ${PROJECT_SOURCE_DIR}/mmgr/file-be)

install(FILES "${PROJECT_SOURCE_DIR}/host/nvme-host.h"
	      "${PROJECT_SOURCE_DIR}/host/nvme-host-dev.h"
              "${PROJECT_SOURCE_DIR}/transport/ox-fabrics/ox-fabrics.h"
              "${PROJECT_SOURCE_DIR}/include/libox.h"
              "${PROJECT_SOURCE_DIR}/include/nvme.h"
              "${PROJECT_SOURCE_DIR}/include/nvmef.h"
              "${PROJECT_SOURCE_DIR}/include/ox-mq.h"
              "${PROJECT_SOURCE_DIR}/ftl/ox-app/ox-app.h"
              "${PROJECT_SOURCE_DIR}/include/ox-lightnvm.h"
              "${PROJECT_SOURCE_DIR}/include/ox-uatomic.h"
DESTINATION include COMPONENT dev)

# OX Core Library

set(SRC_UTIL
        ${PROJECT_SOURCE_DIR}/core/ox-mq.c
        ${PROJECT_SOURCE_DIR}/core/ox-mq-output.c
        ${PROJECT_SOURCE_DIR}/core/ox-memory.c
        ${PROJECT_SOURCE_DIR}/core/ox-stats.c)
add_library ( ox-util STATIC ${SRC_UTIL} )
install(TARGETS ox-util DESTINATION lib COMPONENT lib)

set(SRC_CORE
        ${PROJECT_SOURCE_DIR}/core/core-init.c
        ${PROJECT_SOURCE_DIR}/core/core-exec.c
        ${PROJECT_SOURCE_DIR}/core/cmd_args.c
        ${PROJECT_SOURCE_DIR}/core/ox_cmdline.c
        ${PROJECT_SOURCE_DIR}/core/admin.c
        ${PROJECT_SOURCE_DIR}/core/nvme_ctrl.c
        ${PROJECT_SOURCE_DIR}/core/nvmef_ctrl.c
        ${PROJECT_SOURCE_DIR}/core/lightnvm.c
        ${PROJECT_SOURCE_DIR}/mmgr/mmgr_common.c
        ${PROJECT_SOURCE_DIR}/ftl/ftl_common.c)

add_library ( ox STATIC ${SRC_CORE} )
target_link_libraries ( ox readline )
target_link_libraries ( ox m )
target_link_libraries ( ox ox-util )
target_link_libraries ( ox ox-ftl-lnvm )
target_link_libraries ( ox ox-ftl-oxapp )
install(TARGETS ox DESTINATION lib COMPONENT lib)

# OX Media manager Libraries

set(SRC_MMGR_FPGA
        ${PROJECT_SOURCE_DIR}/mmgr/fpga-3.0.1/fpga-3_0_1.c
        ${PROJECT_SOURCE_DIR}/mmgr/fpga-3.0.1/nand_dma.c)
add_library ( ox-mmgr-fpga STATIC ${SRC_MMGR_FPGA} )
target_link_libraries ( ox-mmgr-fpga ox )
install(TARGETS ox-mmgr-fpga DESTINATION lib COMPONENT lib)

set(SRC_MMGR_VOLT ${PROJECT_SOURCE_DIR}/mmgr/volt/volt.c)
add_library ( ox-mmgr-volt STATIC ${SRC_MMGR_VOLT} )
target_link_libraries ( ox-mmgr-volt ox )
install(TARGETS ox-mmgr-volt DESTINATION lib COMPONENT lib)

set(SRC_MMGR_FILEBE ${PROJECT_SOURCE_DIR}/mmgr/file-be/file-be.c)
add_library ( ox-mmgr-filebe STATIC ${SRC_MMGR_FILEBE} )
target_link_libraries ( ox-mmgr-filebe ox )
install(TARGETS ox-mmgr-filebe DESTINATION lib COMPONENT lib)

# UNCOMMENT FOR OPEN-CHANNEL SSD V1.2 SUPPORT (first, install liblightnvm v0.1.2)
#set(SRC_MMGR_OCSSD_1_2 ${PROJECT_SOURCE_DIR}/mmgr/ocssd-1.2/ocssd-1_2.c)
#add_library ( ox-mmgr-ocssd-1.2 STATIC ${SRC_MMGR_OCSSD_1_2} )
#target_link_libraries ( ox-mmgr-ocssd-1.2 ox )
#target_link_libraries ( ox-mmgr-ocssd-1.2 lightnvm )
#install(TARGETS ox-mmgr-ocssd-1.2 DESTINATION lib COMPONENT lib)

# OX Flash Translation Layers Libraries

set(SRC_FTL_LNVM
        ${PROJECT_SOURCE_DIR}/ftl/lnvm/lnvm_ftl.c
        ${PROJECT_SOURCE_DIR}/ftl/lnvm/lnvm_bbtbl.c)
add_library ( ox-ftl-lnvm STATIC ${SRC_FTL_LNVM} )
target_link_libraries ( ox-ftl-lnvm ox )
install(TARGETS ox-ftl-lnvm DESTINATION lib COMPONENT lib)

set(SRC_FTL_OXAPP
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/app-core.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/app-channels.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/app-transaction.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/app-hmap.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/app-cache.c)
add_library ( ox-ftl-oxapp STATIC ${SRC_FTL_OXAPP} )
target_link_libraries ( ox-ftl-oxapp ox )
install(TARGETS ox-ftl-oxapp DESTINATION lib COMPONENT lib)

set(SRC_FTL_OXBLK
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-bbtbl.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-blk-md.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-ch-map.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-gl-map.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-ch-prov.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-gl-prov.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-ppa-io.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-lba-io.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-gc.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-log.c
        ${PROJECT_SOURCE_DIR}/ftl/ox-app/block/oxb-recovery.c
	${PROJECT_SOURCE_DIR}/ftl/ox-app/block/non-recoverable/oxb-chmap-nr.c
	${PROJECT_SOURCE_DIR}/ftl/ox-app/block/non-recoverable/oxb-blkmd-nr.c)
add_library ( ox-ftl-block STATIC ${SRC_FTL_OXBLK} )
target_link_libraries ( ox-ftl-block ox )
target_link_libraries ( ox-ftl-block ox-ftl-oxapp )
install(TARGETS ox-ftl-block DESTINATION lib COMPONENT lib)

# OX Command Parser Libraries

set(SRC_PARSER_SPEC
        ${PROJECT_SOURCE_DIR}/parser/nvme_parser.c
        ${PROJECT_SOURCE_DIR}/parser/lnvm_parser.c
        ${PROJECT_SOURCE_DIR}/parser/fabrics_parser.c)
add_library ( ox-parser-spec STATIC ${SRC_PARSER_SPEC} )
target_link_libraries ( ox-parser-spec ox )
install(TARGETS ox-parser-spec DESTINATION lib COMPONENT lib)

# OX NVMe Transport Libraries

set(SRC_TRANSP_PCI_FPGA
        ${PROJECT_SOURCE_DIR}/transport/pci_fpga_3.0.1/pci_fpga_301.c)
add_library ( ox-transport-pci-fpga STATIC ${SRC_TRANSP_PCI_FPGA} )
target_link_libraries ( ox-transport-pci-fpga ox )
install(TARGETS ox-transport-pci-fpga DESTINATION lib COMPONENT lib)

set(SRC_TRANSP_FABRICS_H
        ${PROJECT_SOURCE_DIR}/transport/ox-fabrics/udp-client.c
        ${PROJECT_SOURCE_DIR}/transport/ox-fabrics/tcp-client.c
        ${PROJECT_SOURCE_DIR}/transport/ox-fabrics/ox-host.c)
add_library ( ox-fabrics-host STATIC ${SRC_TRANSP_FABRICS_H} )
target_link_libraries ( ox-fabrics-host ox )
install(TARGETS ox-fabrics-host DESTINATION lib COMPONENT lib)

set(SRC_TRANSP_FABRICS_T
        ${PROJECT_SOURCE_DIR}/transport/ox-fabrics/udp-server.c
        ${PROJECT_SOURCE_DIR}/transport/ox-fabrics/tcp-server.c
        ${PROJECT_SOURCE_DIR}/transport/ox-fabrics/ox-fabrics.c)
add_library ( ox-transport-fabrics-tgt STATIC ${SRC_TRANSP_FABRICS_T} )
target_link_libraries ( ox-transport-fabrics-tgt ox )
install(TARGETS ox-transport-fabrics-tgt DESTINATION lib COMPONENT lib)

# OX Controller Targets

 # Volatile Media Manager
if (VOLT_GB)
    add_definitions (-DCONFIG_VOLT_GB=${VOLT_GB})
else()
    add_definitions (-DCONFIG_VOLT_GB=4)
endif()

set(OX_NVME_VOLT ${PROJECT_SOURCE_DIR}/targets/ox-ctrl-nvme-volt.c )
add_executable ( ox-ctrl-nvme-volt ${OX_NVME_VOLT} )
target_link_libraries ( ox-ctrl-nvme-volt ox )
target_link_libraries ( ox-ctrl-nvme-volt ox-mmgr-volt )
target_link_libraries ( ox-ctrl-nvme-volt ox-ftl-lnvm )
target_link_libraries ( ox-ctrl-nvme-volt ox-ftl-block )
target_link_libraries ( ox-ctrl-nvme-volt ox-parser-spec )
target_link_libraries ( ox-ctrl-nvme-volt ox-transport-fabrics-tgt )

 # File Backed Media Manager
if (FILE_GB)
    add_definitions (-DCONFIG_FILE_GB=${FILE_GB})
else()
    add_definitions (-DCONFIG_FILE_GB=4)
endif()

if (FILE_FOLDER)
    add_definitions (-DCONFIG_FILE_FOLDER="${FILE_FOLDER}")
else()
    add_definitions (-DCONFIG_FILE_FOLDER=".")
endif()

set(OX_NVME_FILEBE ${PROJECT_SOURCE_DIR}/targets/ox-ctrl-nvme-filebe.c )
add_executable ( ox-ctrl-nvme-filebe ${OX_NVME_FILEBE} )
target_link_libraries ( ox-ctrl-nvme-filebe ox )
target_link_libraries ( ox-ctrl-nvme-filebe ox-mmgr-filebe)
target_link_libraries ( ox-ctrl-nvme-filebe ox-ftl-lnvm )
target_link_libraries ( ox-ctrl-nvme-filebe ox-ftl-block )
target_link_libraries ( ox-ctrl-nvme-filebe ox-parser-spec )
target_link_libraries ( ox-ctrl-nvme-filebe ox-transport-fabrics-tgt )

#UNCOMMENT FOR OPEN-CHANNEL SSD V1.2 SUPPORT (first, install liblightnvm v0.1.2)
#set(OX_NVME_OCSSD ${PROJECT_SOURCE_DIR}/targets/ox-ctrl-nvme-ocssd.c )
#add_executable ( ox-ctrl-nvme-ocssd ${OX_NVME_OCSSD} )
#target_link_libraries ( ox-ctrl-nvme-ocssd ox )
#target_link_libraries ( ox-ctrl-nvme-ocssd ox-mmgr-ocssd-1.2 )
#target_link_libraries ( ox-ctrl-nvme-ocssd ox-ftl-lnvm )
#target_link_libraries ( ox-ctrl-nvme-ocssd ox-ftl-block )
#target_link_libraries ( ox-ctrl-nvme-ocssd ox-parser-spec )
#target_link_libraries ( ox-ctrl-nvme-ocssd ox-transport-fabrics-tgt )

# Host Applications

set(SRC_NVME_HOST
        ${PROJECT_SOURCE_DIR}/host/nvme_host.c)
add_library ( ox-host-nvme STATIC ${SRC_NVME_HOST} )
target_link_libraries ( ox-host-nvme ox-fabrics-host )
install(TARGETS ox-host-nvme DESTINATION lib COMPONENT lib)

set(OX_TEST_CONNECT ${PROJECT_SOURCE_DIR}/test/test-connect.c )
add_executable ( ox-test-connect ${OX_TEST_CONNECT} )
target_link_libraries ( ox-test-connect ox-fabrics-host )

set(OX_TEST_QUEUE ${PROJECT_SOURCE_DIR}/test/test-queue.c )
add_executable ( ox-test-queue ${OX_TEST_QUEUE} )
target_link_libraries ( ox-test-queue ox-fabrics-host )

set(OX_TEST_NVME_RW ${PROJECT_SOURCE_DIR}/test/test-nvme-rw.c )
add_executable ( ox-test-nvme-rw ${OX_TEST_NVME_RW} )
target_link_libraries ( ox-test-nvme-rw ox-host-nvme )

set(OX_TEST_OX_MQ ${PROJECT_SOURCE_DIR}/test/test-ox-mq.c )
add_executable ( ox-test-mq ${OX_TEST_OX_MQ} )
target_link_libraries ( ox-test-mq ox )

set(OX_TEST_NVME_THPUT_W ${PROJECT_SOURCE_DIR}/test/test-nvme-thput-w.c )
add_executable ( ox-test-nvme-thput-w ${OX_TEST_NVME_THPUT_W} )
target_link_libraries ( ox-test-nvme-thput-w ox-host-nvme )

set(OX_TEST_NVME_THPUT_R ${PROJECT_SOURCE_DIR}/test/test-nvme-thput-r.c )
add_executable ( ox-test-nvme-thput-r ${OX_TEST_NVME_THPUT_R} )
target_link_libraries ( ox-test-nvme-thput-r ox-host-nvme )

add_definitions (-D_FILE_OFFSET_BITS=64)
set(OX_FUSE_BIGFILE
    ${PROJECT_SOURCE_DIR}/fuse/fuse-bigfile.c
    ${PROJECT_SOURCE_DIR}/fuse/fuse-ox-nvme.c )
add_executable ( ox-fuse-bigfile ${OX_FUSE_BIGFILE} )
target_link_libraries ( ox-fuse-bigfile fuse )
target_link_libraries ( ox-fuse-bigfile ox-host-nvme )
