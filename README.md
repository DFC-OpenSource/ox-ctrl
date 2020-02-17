# OX: Computational Storage SSD Controller

OX is (I) a computational storage solution for high-performance applications, (II) a framework for application-specific FTL development, (III) an NVMe controller that exposes open-channel SSDs as a block device via OX-Block FTL, and (IV) a set of libraries that integrate NVMe over Fabrics and Computational Storage with host applications.

```
RELEASE
OX 2.6 is released. This version of OX contains the full-fledged FTL OX-Block. The FTL maintains
a 4KB-granularity mapping table, implements garbage collection, and performs write-ahead logging,
checkpoint, and recovery. OX 2.6 also comes with our NVMe over Fabrics implementation (using TCP sockets),
future versions are expected with RDMA/Infiniband support. The last new feature is computational
storage support, OX provides a set of libraries and interfaces for offloading applications to storage
devices running OX.
OX 2.6 runs in the DFC, other ARM platforms (Broadcom Stingray), and any x86 machines.

OLD RELEASES
OX 1.4.0 is released. The main feature is supporting pblk target (host FTL) by enabling 
sector-granularity reads. It allows file systems on top of the DFC namespace. It requires 
the linux kernel 4.13 or higher. Please, check the 'pblk: Host-side FTL setup' section.
```
**Website:**  
  COMING SOON...

**Documentation:**  
- https://github.com/DFC-OpenSource/ox-ctrl/wiki  
- [OX PhD Thesis by Ivan Picoli](https://itu.dk/research/dfc-data/IvanPicoli-PhDThesis-08-Jul-19-final.pdf)

**Publications:**  

[- uFLIP-OC: Understanding Flash I/O Patterns on Open-Channel Solid-State Drives](https://dl.acm.org/citation.cfm?id=3124680.3124741), ApSys '17

[- Improving CPU I/O Performance via SSD Controller FTL Support for Batched Writes](https://dl.acm.org/citation.cfm?id=3329925), DaMoN '19

[- LSM Management on Computational Storage](https://dl.acm.org/citation.cfm?id=3329927), DaMoN '19

[- Programming Storage Controllers with OX](http://nvmw.ucsd.edu/nvmw2019-program/unzip/current/nvmw2019-final54.pdf), NVMW '19

[- Open-Channel SSD (What is it Good For)](http://cidrdb.org/cidr2020/papers/p17-picoli-cidr20.pdf), CIDR'20

OX was developed under a project at IT University of Copenhagen. Further work was done for BwTree computational storage at Microsoft Research. Currently, LSM-Tree computational storage techniques are under development at ITU Copenhagen.  

Detailed information can be found here (https://github.com/DFC-OpenSource/ox-ctrl/wiki), latest releases here (https://github.com/DFC-OpenSource/ox-ctrl/releases) and code under development here (https://github.com/DFC-OpenSource/ox-ctrl/tree/for-next).


# Installation:

```
 Possible Ubuntu packages:
 $ sudo apt-get install cmake libreadline6 libreadline6-dev
 
 Install OX:
 $ git clone https://github.com/DFC-OpenSource/ox-ctrl.git
 $ cd ox-ctrl
 $ mkdir build
 $ cd build
 $ cmake -DVOLT_GB=<4,8,16,32,64> ..   (If you are using DRAM backend, set -DVOLT_GB accordingly)
 $ sudo make install
```

# Localhost Testing:

```
 - Open 2 terminals.
 First terminal:
   $ cd <ox-ctrl>/build
   $ ./ox-ctrl-nvme-volt start
   Wait until you see OX startup
   Try some commands:
    > help
    > show memory
    > show io
    > show gc
    > show cp
    > debug on
    > debug off
    > show mq status
    > exit (if you want to close OX)
 Second terminal:
   $ cd <ox-ctrl>/build
   Write something to your OX device:
     (Here you are writing BLKS 4K-blocks starting from block SLBA)
     $ ./ox-test-nvme-thput-w <SLBA> <BLKS>
     (Here you are writing 10000 4K-blocks starting from block 1)
     $ ./ox-test-nvme-thput-w 1 10000
   Read what you wrote:
     $ ./ox-test-nvme-thput-r 1 10000

You can type 'show io' in the first terminal while running the tests for runtime statistics.
   
```

# Application integration
 - SOON


# Internal components:

```   - Open-channel SSD v1.2 media manager
      - DRAM media manager (VOLT)
      - LightNVM FTL support for raw open-channel SSD access
      - OX-App Application-specific FTL Framework
      - OX-Block full-fledged FTL (Block device)
      - NVMe over PCI controller
      - NVMe over Fabrics controller
      - TCP-based fabrics implementation
      - OX NVMe over fabrics user-space library
      - OX-Block NVMe Computational Storage user-space library
```

# OX command line

$ ox-ctrl-nvme-volt <command>
```
Start OX in a specific mode. Commands are listed with --help, as shown below.
```
$ ox-ctrl-nvme-volt --help
```
*** OX Controller 2.6 - The Ultimate Dragon ***
 
 The DFC Open-Channel SSD Controller

 Available commands:
  start            Start controller with standard configuration
  debug            Start controller and print Admin/IO commands
  reset            Start controller and reset FTL
                       WARNING (reset): ALL DATA WILL BE LOST
  admin            Execute specific tasks within the controller
 
 Initial release developed by Ivan L. Picoli <ivpi@itu.dk>


  -?, --help                 Give this help list
      --usage                Give a short usage message
  -V, --version              Print program version

Report bugs to Ivan L. Picoli <ivpi@itu.dk>.

```
The same commands apply for VOLT media menager (ox-ctrl-volt).

$ ox-ctrl-nvme-volt admin --help
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

$ ox-ctrl-nvme-volt admin -l
```
OX Controller ADMIN
  Available OX Admin Tasks: 

  - 'erase-blk': erase specific blocks.
     eg. ox-ctrl admin -t erase-blk

  - 'create-bbt': create or update the bad block table.
     eg. ox-ctrl admin -t create-bbt (see below)
```

# Bad block table creation
The bad block table is an essential component of a flash block device, without it you can expect data corruption due
application bad block allocation. OX has an admin command for creating per-channel bad block tables. Please follow the instruction in 'ox-ctrl-test admin -t create-bbt' command.

When attempting to create a bad block table in a given channel, OX will ask which process you want to use. There are 3 available processes:
``` 
   1 - Full scan (Erase, write full, read full, compare buffers) - RECOMMENDED. Might take a long time
           - Backup the channel data before. ALL DATA WILL BE LOST.
           
   2 - Fast scan (Only erase the blocks) - Fast creation, but can have omitted bad blocks.
           - Backup the channel data before. ALL DATA WILL BE LOST.
           
   3 - Emergency table (Creates an empty bad block table without erasing blocks) - Very fast and the channel is not erased.
           - If you can't make a backup of your channel, use this option for initializing OX, however, all blocks will be
              marked as good. You should use the DFC in read-only state if you create an emergency bad block table.
```
