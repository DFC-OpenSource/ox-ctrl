# OX: Open-Channel SSD Controller

OX is a controller solution for programmable devices like the dragon Fire Card. OX exposes the
device as a LightNVM compatible Open-Channel SSD. OX has been developed to work
as a hybrid controller, potentially supporting different FTL responsabilities (e.g. write
buffering or ECC) or a full-fledged FTL. FTLs are registered within the OX core,
enabling applications to select channels to be managed by a specific FTL. Within a
device we may have different channels managed by different FTLs.

The repositories cited here are the latest setup tested succesfully with OX. Once direct I/O support to physical Open-Channel SSDs in the Linux kernel is a work-in-progress task, mix the setup with other sources may result in compatibility problems.

For the latest commits, plase check branch 'for-next'.

PLEASE, REFER TO THE WIKI FOR FULL DOCUMENTATION:
```
https://github.com/DFC-OpenSource/ox-ctrl/wiki
```
CHECK RELEASES:
```
https://github.com/DFC-OpenSource/ox-ctrl/releases
```
IF YOU DO NOT HAVE AN OPEN-CHANNEL SSD, YOU CAN USE QEMU WITH AN EMBEDDED OX CONTROLLER:
```
https://github.com/DFC-OpenSource/qemu-ox
```
STABLE KERNEL FOR USER PPA IO THROUGHT LIBLIGHTNVM:
```
https://github.com/ivpi/linux-liblnvm
```
KERNEL WITH A HOST-BASED FTL FOR OPEN-CHANNEL SSDS (PBLK):
```
https://github.com/OpenChannelSSD/linux/tree/pblk.19
```
OX-ENABLED NVME LINUX DRIVER:
```
https://github.com/ivpi/nvme-driver-DFC
```
USER-SPACE LIBRARY FOR OPEN-CHANNEL SSDs (LIBLIGHTNVM):
```
https://github.com/ivpi/liblightnvm
```
NVME CLIENT FOR OPEN-CHANNEL SSD MANAGEMENT (nvme-cli):
```
https://github.com/linux-nvme/nvme-cli
```
TOOL FOR TESTING OPEN-CHANNEL SSDs:
```
https://github.com/ivpi/fox
```
SETTING UP THE ENVIRONMENT FOR USER PPA IOs:
```
- Install the kernel for user ppa IO;
  - https://github.com/ivpi/linux-liblnvm  
- Blacklist the nvme driver (not necessary in QEMU. For the DFC, we use an OX-enabled driver);
- Install liblightnvm;
  - https://github.com/ivpi/linux-liblnvm
- Install nvme-cli;
  - https://github.com/linux-nvme/nvme-cli
- Make sure gennvm module is loaded, if not, load it;
  $ sudo modprobe gennvm;
- Start OX Controller in the DFC (in QEMU it will be already started). You have to install OX on the DFC before;
  In the DFC console:
  $ ox-ctrl start
- Build and load the NVMe driver (in QEMU it will be already loaded);
  - https://github.com/ivpi/nvme-driver-DFC
  $ sudo insmod <driver_folder>/nvme-core.ko
  $ sudo insmod <driver_folder>/nvme.ko
- Check the kernel log, you should see the device registration messages;
  $ dmesg
- Initialize the device with nvme-cli:
  $ sudo nvme lnvm init -d nvme0n1  
- Check the device with nvme-cli, you should see the device with 'gennvm' initialized in nvme0n1;
  $ sudo nvme lnvm list
- Run the tests with the tool (https://github.com/ivpi/fox), or use liblightnvm as you wish.
```
UBUNTU IMAGE WITH THE ENVIRONMENT AND TESTS READY TO RUN:
```
soon...
```

LIMITATIONS:
```
OX DOES NOT HAVE A FTL FOR STANDARD BLOCK DEVICES, BUT IT HAS THE CAPABILITIES FOR IT. FOR NOW OX WORKS ONLY AS OPEN-CHANNEL CONTROLLER.
OX HAS BEEN DESIGNED TO SUPPORT SEVERAL FTL IMPLEMENTATIONS IN A STANDARD INTERFACE.
IT IS FUTURE WORK. YOU ARE WELCOME TO CONTRIBUTE.
```

This controller supports the FPGA 3.01.00.

The latest stable DFC firmware version:

```
========================================
        Image           Version                
 ========================================
    PBL                   03.00.00
    DPC                   03.00.00
    MC                    03.00.00
    Kernel                05.00.00
    U-boot                03.00.00
    rootfs                03:00:00
    CPLD_NIC              A12 
    CPLD_SSD              A02 
    FPGA                  03.01.00

```
The latest DFC hardware configuration:
```
2 NAND DIMM modules installed in the slots M1 and M3 of the storage card.
```
Features:

```
      - Media managers: NAND, DDR, etc.
      - Flash Translation Layers + LightNVM raw FTL support
      - Interconnect handler: PCIe, network fabric, etc.
      - NVMe queues and tail/head doorbells
      - NVMe, LightNVM and Fabric(not implemented) command parser
      - RDMA handler(not implemented): RoCE, InfiniBand, etc.

      Media managers (MMGR): Identifies the non-volatile memory, and registers a channel
        abstraction with its geometry to be available to the stack. Media managers
        implement read/write/erase commands to specific pages and blocks. OX may have
        several media managers exposing channels from different sources.
        
      Flash Translation Layers (FTL): Manages MMGR channels, accepts IO commands (a
        command may have several pages) and sends page-level commands to the media
        managers. It implements responsibilities such as WL, L2P, GC, BB and ECC. Several
        FTLs may run within OX and be responsible to manage different MMGR channels.

      Interconnect handlers (ICH): Communication between controller and host. This layer
        implements responsibilities such as NVMe register mapping and MSI interruptions. It
        gathers data from the host and sends it to the command parser layer.
        
      NVMe queue support: Read/write NVMe registers mapped by the ICH, manage admin/
        IO NVMe queue(s) and perform the DMA data transfer.
        
      Command parsers: Parses commands coming from the ICH and call the right FTL
        instance to handle it.
    
        Since media managers expose NAND as channels, applications can make 
    bindings between channels and FTLs. It means that inside a device we can 
    have different channels managed by different FTLs.
```
We have implemented a PCIe interconnection handler, a media manager to expose 8 channels, NVMe and LightNVM support for the DFC.

HOW TO INSTALL AND USE:

The Makefile creates 2 binaries (ox-ctrl and ox-ctrl-test). ox-ctrl is a essential-only binary for a production environment. ox-ctrl-test is a complete binary including all tests and administration mode.

Under /bin you can find both binaries + the DFC rootfs with OX included. 

WE RECOMMEND THE USE OF ROOTFS. AFTER UPGRADE THE ROOTFS FIRMWARE, ONLY TYPE 'ox-ctrl' OR 'ox-ctrl-test' TO USE OX.

OX accepts parameters as follow:

#ox-ctrl start
```
Use 'start' to run the controller with standard settings.
```

#ox-ctrl debug
```
Use 'debug' to run the controller in debug mode (verbose).
```

#ox-ctrl --help
```
*** OX Controller ***
 
 An OpenChannel SSD Controller

 Available commands:
  start            Start controller with standard configuration
  debug            Start controller and print Admin/IO commands
  test             Start controller, run tests and close
  admin            Execute specific tasks within the controller
 
 Initial release developed by Ivan L. Picoli, the red-eagle


  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version

Report bugs to Ivan L. Picoli <ivpi@itu.dk>

```

#ox-ctrl-test test --help
```
Use this command to run tests, it will start the controller, run the tests and
close the controller.

 Examples:
  Show all available set of tests + subtests:
    ox-ctrl test -l

  Run all available tests:
    ox-ctrl test -a

  Run a specific test set:
    ox-ctrl test -s <set_name>

  Run a specific subtest:
    ox-ctrl test -s <set_name> -t <subtest_name>

  -a, --all[=run_all]        Use to run all tests.
  -l, --list[=list]          Show available tests.
  -s, --set=test_set         Test set name. <char>
  -t, --test=test            Subtest name. <char>
  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version

Mandatory or optional arguments to long options are also mandatory or optional
for any corresponding short options.

Report bugs to Ivan L. Picoli <ivpi@itu.dk>.

```

#ox-ctrl-test admin --help
```
Use this command to run specific tasks within the controller.

 Examples:
  Show all available Admin tasks:
    ox-ctrl admin -l

  Run a specific admin task:
    ox-ctrl admin -t <task_name>

  -l, --list[=list]          Show available admin tasks.
  -t, --task=admin_task      Admin task to be executed. <char>
  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version

Mandatory or optional arguments to long options are also mandatory or optional
for any corresponding short options.

Report bugs to Ivan L. Picoli <ivpi@itu.dk>.
```

#ox-ctrl-test admin -l
```
OX Controller ADMIN
 
Available OX Admin Tasks: 

  - 'erase-blk': erase specific blocks.
     eg. ox-ctrl admin -t erase-blk

  - 'erase-lun': erase specific LUNs.
     eg. ox-ctrl admin -t erase-lun (not implemented)

  - 'erase-ch': erase specific channels.
     eg. ox-ctrl admin -t erase-ch (not implemented)
```

#OX layer registration:

```
Besides the DFC, OX is extensible to other devices through the layer registration interface. In order to
use OX in other platforms, it is needed to develop the follow layers:

- Interconnect handler: Here you map the NVMe registers according your platform, or make a Fabrics interconnection.
- Media Manager: Here you abstract the channels of your non-volatile memory as channels, to be used by the other OX layers.
- FTL: You can use the same FTL at any platform, but you can develop a new one and register it within the OX core.
```

#OX Media Manager (MMGR) registration interface:
```
typedef int     (nvm_mmgr_read_pg)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_write_pg)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_erase_blk)(struct nvm_mmgr_io_cmd *);
typedef int     (nvm_mmgr_get_ch_info)(struct nvm_channel *, uint16_t);
typedef int     (nvm_mmgr_set_ch_info)(struct nvm_channel *, uint16_t);
typedef void    (nvm_mmgr_exit)(struct nvm_mmgr *);

struct nvm_mmgr_ops {
    nvm_mmgr_read_pg       *read_pg;
    nvm_mmgr_write_pg      *write_pg;
    nvm_mmgr_erase_blk     *erase_blk;
    nvm_mmgr_exit          *exit;
    nvm_mmgr_get_ch_info   *get_ch_info;
    nvm_mmgr_set_ch_info   *set_ch_info;
};

struct nvm_mmgr_geometry {
    uint8_t     n_of_ch;
    uint8_t     lun_per_ch;
    uint16_t    blk_per_lun;
    uint16_t    pg_per_blk;
    uint16_t    sec_per_pg;
    uint8_t     n_of_planes;
    uint32_t    pg_size;
    uint32_t    sec_oob_sz;
};

struct nvm_channel {
    uint16_t                    ch_id;
    uint16_t                    ch_mmgr_id;
    uint64_t                    ns_pgs;
    uint64_t                    slba;
    uint64_t                    elba;
    uint64_t                    tot_bytes;
    uint16_t                    mmgr_rsv; /* number of blks reserved by mmgr */
    uint16_t                    ftl_rsv;  /* number of blks reserved by ftl */
    struct nvm_mmgr             *mmgr;
    struct nvm_ftl              *ftl;
    struct nvm_mmgr_geometry    *geometry;
    struct nvm_ppa_addr         *mmgr_rsv_list; /* list of mmgr reserved blks */
    struct nvm_ppa_addr         *ftl_rsv_list;
    LIST_ENTRY(nvm_channel)     entry;
    union {
        struct {
            uint64_t   ns_id         :16;
            uint64_t   ns_part       :32;
            uint64_t   ftl_id        :8;
            uint64_t   in_use        :8;
        } i;
        uint64_t       nvm_info;
    };
};

struct nvm_mmgr {
    char                        *name;
    struct nvm_mmgr_ops         *ops;
    struct nvm_mmgr_geometry    *geometry;
    struct nvm_channel          *ch_info;
    LIST_ENTRY(nvm_mmgr)        entry;
};
```

#FTL registration interface:
```
typedef int       (nvm_ftl_submit_io)(struct nvm_io_cmd *);
typedef void      (nvm_ftl_callback_io)(struct nvm_mmgr_io_cmd *);
typedef int       (nvm_ftl_init_channel)(struct nvm_channel *);
typedef void      (nvm_ftl_exit)(struct nvm_ftl *);
typedef int       (nvm_ftl_get_bbtbl)(struct nvm_ppa_addr *,uint8_t *,uint32_t);
typedef int       (nvm_ftl_set_bbtbl)(struct nvm_ppa_addr *, uint32_t);

struct nvm_ftl_ops {
    nvm_ftl_submit_io      *submit_io; /* FTL queue request consumer */
    nvm_ftl_callback_io    *callback_io;
    nvm_ftl_init_channel   *init_ch;
    nvm_ftl_exit           *exit;
    nvm_ftl_get_bbtbl      *get_bbtbl;
    nvm_ftl_set_bbtbl      *set_bbtbl;
};

/* --- FTL CAPABILITIES BIT OFFSET --- */

enum {
    /* Get/Set Bad Block Table support */
    FTL_CAP_GET_BBTBL     = 0x00,
    FTL_CAP_SET_BBTBL     = 0x01,
    /* Get/Set Logical to Physical Table support */
    FTL_CAP_GET_L2PTBL    = 0x02,
    FTL_CAP_SET_L2PTBL    = 0x03,
};

/* --- FTL BAD BLOCK TABLE FORMATS --- */

enum {
   /* Each block within a LUN is represented by a byte, the function must return
   an array of n bytes, where n is the number of blocks per LUN. The function
   must set single bad blocks or accept an array of blocks to set as bad. */
    FTL_BBTBL_BYTE     = 0x00,
};

struct nvm_ftl {
    uint16_t                ftl_id;
    char                    *name;
    struct nvm_ftl_ops      *ops;
    uint32_t                cap; /* Capability bits */
    uint16_t                bbtbl_format;
    uint8_t                 nq; /* Number of queues/threads, up to 64 per FTL */
    uint16_t                mq_id[64];
    pthread_t               io_thread[64];
    uint8_t                 last_q;
    pthread_mutex_t         q_mutex;
    uint8_t                 active;
    LIST_ENTRY(nvm_ftl)     entry;
};
```

#Interconnect handler registration interface:
```
typedef void        (nvm_pcie_isr_notify)(void *);
typedef void        (nvm_pcie_exit)(void);
typedef void        *(nvm_pcie_nvme_consumer) (void *);

struct nvm_pcie_ops {
    nvm_pcie_nvme_consumer  *nvme_consumer;
    nvm_pcie_isr_notify     *isr_notify; /* notify host about completion */
    nvm_pcie_exit           *exit;
};

struct nvm_pcie {
    char                        *name;
    void                        *ctrl;            /* pci specific structure */
    union NvmeRegs              *nvme_regs;
    struct nvm_pcie_ops          *ops;
    struct nvm_memory_region    *host_io_mem;     /* host BAR */
    pthread_t                   io_thread;        /* single thread for now */
    uint32_t                    *io_dbstride_ptr; /* for queue scheduling */
};
```

#OX Core Global Functions (used by all layers):
```
/* core functions */
int  nvm_register_mmgr(struct nvm_mmgr *);
int  nvm_register_pcie_handler(struct nvm_pcie *);
int  nvm_register_ftl (struct nvm_ftl *);
int  nvm_submit_io (struct nvm_io_cmd *);
int  nvm_submit_mmgr_io (struct nvm_mmgr_io_cmd *);
void nvm_callback (struct nvm_mmgr_io_cmd *);
void nvm_complete_io (struct nvm_io_cmd *);
int  nvm_dma (void *, uint64_t, ssize_t, uint8_t);
int  nvm_memcheck (void *);
int  nvm_ftl_cap_exec (uint8_t, void **, int);
int  nvm_init_ctrl (int, char **);
int  nvm_test_unit (struct nvm_init_arg *);
int  nvm_submit_sync_io (struct nvm_channel *, struct nvm_mmgr_io_cmd *,
                                                              void *, uint8_t);
int  nvm_submit_multi_plane_sync_io (struct nvm_channel *,
                          struct nvm_mmgr_io_cmd *, void *, uint8_t, uint64_t);
```
