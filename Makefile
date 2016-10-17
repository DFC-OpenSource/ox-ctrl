######### Makefile for OX: OpenChannel SSD Controller #########

NAME = ox-ctrl
NAMET = ox-ctrl-test
CORE = core.o nvme.o nvme_cmd.o lightnvm.o cmd_args.o
CLEAN = *.o

### MEDIA MANAGERS
DFCNAND_PATH = mmgr/dfc_nand
DFCNAND = $(DFCNAND_PATH)/dfc_nand.o $(DFCNAND_PATH)/nand_dma.o
CLEAN += $(DFCNAND_PATH)/*.o
#Join media managers in MMGRS
MMGRS_DFC = $(DFCNAND)

### FLASH TRANSLATION LAYERS
LNVM_FTL_PATH = ftl/lnvm
LNVM_FTL = $(LNVM_FTL_PATH)/ftl_lnvm.o
CLEAN += $(LNVM_FTL_PATH)/*.o
#Join flash translation layers in FTLS
FTLS_DFC = $(LNVM_FTL)

### INTERCONNECT HANDLERS
PCIE_DFC_PATH = pcie_dfc
PCIE_DFC = $(PCIE_DFC_PATH)/pcie_dfc.o
CLEAN += $(PCIE_DFC_PATH)/*.o

### TESTS
TESTS_DFC_PATH = test
TESTS_DFC = $(TESTS_DFC_PATH)/test_core.o $(TESTS_DFC_PATH)/test_mmgr.o
TESTS_DFC += $(TESTS_DFC_PATH)/test_lightnvm.o $(TESTS_DFC_PATH)/test_admin.o
CLEAN += $(TESTS_DFC_PATH)/*.o

### GLOBAL FLAGS
#CC = #For yocto project CC comes from Yocto Makefile
CFLAGS = -g
CFLAGS += -lrt
CFLAGSXX = -pg -fPIC -shared -c -o
DEPS = include/ssd.h include/nvme.h include/lightnvm.h

### CORE
%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

### DFC NAND MEDIA MANAGER
$(DFCNAND_PATH)/%.o : %.c $(DEPS)
	$(CC) $(CFLAGSXX) $@ $< $(CFLAGS) $(DFCNAND_PATH)/nand_dm.a

### LNVM FTL
$(LNVM_FTL_PATH)/%.o : %.c $(DEPS)
	$(CC) $(CFLAGSXX) $@ $< $(CFLAGS)

### DFC PCIe INTERCONNECT
$(PCIE_DFC_PATH)/%.o : %.c $(DEPS)
	$(CC) $(CFLAGSXX) $@ $< $(CFLAGS)

### DFC TESTS
$(TESTS_DFC_PATH)/%.o : %.c include/tests.h
	$(CC) $(CFLAGSXX) $@ $< $(CFLAGS) -Wall

.PHONY: all clean

### TARGETS ###
all: dfc dfc-tests

dfc: $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC)
	$(CC) $(CFLAGS) $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) -o $(NAME) -lpthread

dfc-tests: $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC)
	$(CC) $(CFLAGS) $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC) -o $(NAMET) -lpthread

clean:
	rm -f $(CLEAN) $(NAME) $(NAMET)
