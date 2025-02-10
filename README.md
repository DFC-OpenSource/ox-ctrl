# OX: Computational Storage SSD Controller

OX is (I) a computational storage solution for high-performance applications, (II) a framework for application-specific FTL development, (III) an NVMe controller that exposes open-channel SSDs as a block device via OX-Block FTL, and (IV) a set of libraries that integrate NVMe over Fabrics and Computational Storage with host applications.

```
RELEASE

OX 2.7: This release introduces a new media manager (File Backend), enabling the use of files to simulate
flash memory with up to 4 TiB of storage. Additionally, OX 2.7 now supports a FUSE-based file system,
allowing OX media to be mounted as a standard file system. We've also fixed several bugs to enhance
stability and performance :)

OLD RELEASES
OX 2.6: This release introduces the fully featured FTL OX-Block, which includes a 4KB-granularity
mapping table, garbage collection, write-ahead logging, checkpointing, and recovery. Additionally,
OX 2.6 debuts our NVMe over Fabrics implementation using TCP sockets, with future versions planned
to support RDMA/Infiniband. Another key addition is computational storage support, providing libraries
and interfaces for offloading applications directly to storage devices running OX. OX 2.6 is compatible
with DFC, Broadcom Stingray (ARM platforms), and x86 machines.

OX 1.4 : The main feature is supporting pblk target (host FTL) by enabling 
sector-granularity reads. It allows file systems on top of the DFC namespace. It requires 
the linux kernel 4.13 or higher. Please, check the 'pblk: Host-side FTL setup' section.
```

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
 $ sudo apt-get install cmake libreadline6 libreadline6-dev libfuse-dev
 
 Install OX:
 $ git clone https://github.com/DFC-OpenSource/ox-ctrl.git
 $ cd ox-ctrl
 $ mkdir build
 $ cd build
 $ mkdir disk
 $ cmake -DFILE_FOLDER=./disk -DFILE_GB=<256, 512, 1024, 1536, 2048, 3072, 4096> ..    (If you are using File backend, set -DFILE_GB accordingly)
 OR
 $ cmake -DVOLT_GB=<4,8,16,32,64> ..   (If you are using DRAM backend, set -DVOLT_GB accordingly)
 $ sudo make install
```

# Localhost Testing:

```
 - Open 2 terminals.
 First terminal:
   $ cd <ox-ctrl>/build
   $ ./ox-ctrl-nvme-filebe start
   OR
   $ ./ox-ctrl-nvme-volt start
   Wait until you see OX startup
   Try some commands:
    > help
    > show memory
    > show io
    > show gc
    > show cp
    > show mq status
    > exit (if you want to close OX)
 Second terminal:
   $ cd <ox-ctrl>/build
   Write something to your OX device:
     (Here you are writing BLKS 4K-blocks starting from block SLBA)
     $ ./ox-test-nvme-thput-w <SLBA> <BLKS>
     (Here you are writing 1000000 4K-blocks starting from block 1)
     $ ./ox-test-nvme-thput-w 1 1000000
   Read what you wrote:
     $ ./ox-test-nvme-thput-r 1 1000000

You can type 'show io' in the first terminal while running the tests for runtime statistics.
   
```

# Internal components:

```
      - Open-channel SSD v1.2 media manager
      - DRAM media manager (VOLT)
      - File BE Media Manager (FILE-BE)
      - FUSE File System support
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
