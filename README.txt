# OpenChannel NVM Express SSD Controller

A Generic OpenChannel SSD controller for Application-Driven Storage

Features:
    Media managers: NAND, DDR, etc.
    Flash Translation Layers
    Interconnect handler: PCIe, network fabric, etc.
    NVMe, LightNVM and Fabric(not implemented) command parser
    RDMA handler(not implemented): RoCE, InfiniBand, etc.

    -Media managers are responsible to manage the flash storage and expose
     channels + its geometry (LUNs, blocks, pages, page_size).
 
    -Flash Translation Layers are responsible to manage channels and
     send IOs to media managers.
 
    -Interconnect handlers are responsible to receive NVMe, LightNVM and
     Fabric commands from the interconnection (PCIe or Fabrics) and send to
     command parser.
  
    -Command parsers are responsible to parse Admin (and execute it) and 
     IO (send to the FTL) commands coming from the interconnection. It is 
     possible to set the device as NVMe or LightNVM (OpenChannel device).
    
        Since media managers expose NAND as channels, applications can make 
    bindings between channels and FTLs. It means that inside a device we can 
    have different channels managed by different FTLs.