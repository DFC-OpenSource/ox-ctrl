######### Makefile for OX: OpenChannel SSD Controller #########

NAME = ox-ctrl       # DFC with DFCNAND
NAMET = ox-ctrl-test # DFC with DFCNAND + tests
NAMEV = ox-ctrl-volt # DFC with VOLT + tests
CORE = core.o ox-mq.o nvme.o nvme_cmd.o lightnvm.o cmd_args.o ox_cmdline.o
CORE_VOLT = core-v.o ox-mq-v.o nvme-v.o nvme_cmd-v.o lightnvm-v.o cmd_args-v.o ox_cmdline-v.o
CLEAN = *.o *-v.o

### CONFIGURATION MACROS
CONFIG_FTL = -DCONFIG_FTL_LNVM
# (1) Macro to tell OX core which MMGRs and FTLs are compiled
# (2) Macro to define global Open-Channel geometry
# ox-ctrl and ox-ctrl-test
CONFIG_DFC  = -DCONFIG_MMGR_DFCNAND	    # (1)
CONFIG_DFC += $(CONFIG_FTL)		    # (1)
CONFIG_DFC += -DCONFIG_LNVM_SECSZ=0x1000    # (2)
CONFIG_DFC += -DCONFIG_LNVM_SEC_OOBSZ=0x10  # (2)
CONFIG_DFC += -DCONFIG_LNVM_SEC_PG=4	    # (2)
CONFIG_DFC += -DCONFIG_LNVM_PG_BLK=512	    # (2)
CONFIG_DFC += -DCONFIG_LNVM_CH=8	    # (2)
CONFIG_DFC += -DCONFIG_LNVM_LUN_CH=4	    # (2)
CONFIG_DFC += -DCONFIG_LNVM_BLK_LUN=1024    # (2)
CONFIG_DFC += -DCONFIG_LNVM_PLANES=2	    # (2)
# ox-ctrl-volt
CONFIG_VOLT  = -DCONFIG_MMGR_VOLT		# (1)
CONFIG_VOLT += $(CONFIG_FTL)			# (1)
CONFIG_VOLT += -DCONFIG_LNVM_SECSZ=0x1000	# (2)
CONFIG_VOLT += -DCONFIG_LNVM_SEC_OOBSZ=0x10	# (2)
CONFIG_VOLT += -DCONFIG_LNVM_SEC_PG=4		# (2)
CONFIG_VOLT += -DCONFIG_LNVM_PG_BLK=64		# (2)
CONFIG_VOLT += -DCONFIG_LNVM_CH=8		# (2)
CONFIG_VOLT += -DCONFIG_LNVM_LUN_CH=4		# (2)
CONFIG_VOLT += -DCONFIG_LNVM_BLK_LUN=32		# (2)
CONFIG_VOLT += -DCONFIG_LNVM_PLANES=2		# (2)

### MEDIA MANAGERS
DFCNAND_PATH = mmgr/dfc_nand
DFCNAND = $(DFCNAND_PATH)/dfc_nand.o $(DFCNAND_PATH)/nand_dma.o
VOLT_PATH = mmgr/volt
VOLT = $(VOLT_PATH)/volt.o
CLEAN += $(DFCNAND_PATH)/*.o $(VOLT_PATH)/*.o
# ox-ctrl and ox-ctrl-test
MMGRS_DFC = $(DFCNAND)
# ox-ctrl-volt
MMGRS_VOLT = $(VOLT)

### FLASH TRANSLATION LAYERS
LNVM_FTL_PATH = ftl/lnvm
LNVM_FTL = $(LNVM_FTL_PATH)/ftl_lnvm.o $(LNVM_FTL_PATH)/lnvm_bbtbl.o
CLEAN += $(LNVM_FTL_PATH)/*.o
#Join flash translation layers in FTLS, only LNVM_FTL for now
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
CFLAGS = -g -lrt
CFLAGSXX = -pg -fPIC -shared -c -o
DEPS = include/ssd.h include/nvme.h include/lightnvm.h include/ox-mq.h

### CORE
# ox-ctrl and ox-ctrl-test
%.o: %.c $(DEPS)
	$(CC) $(CONFIG_DFC) -c -o $@ $< $(CFLAGS)
# ox-ctrl-volt
%-v.o: %.c $(DEPS)
	$(CC) $(CONFIG_VOLT) -c -o $@ $< $(CFLAGS)

### DFC NAND MEDIA MANAGER
$(DFCNAND_PATH)/%.o : %.c $(DEPS)
	$(CC) $(CFLAGSXX) $@ $< $(CFLAGS) $(DFCNAND_PATH)/nand_dm.a

### VOLT MEDIA MANAGER
$(VOLT_PATH)/%.o : %.c $(DEPS)
	$(CC) $(CFLAGSXX) $@ $< $(CFLAGS)

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
all: dfc dfc-tests dfc-volt

dfc: $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC)
	$(CC) $(CFLAGS) $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) -o $(NAME) -lpthread -lreadline

dfc-tests: $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC)
	$(CC) $(CFLAGS) $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC) -o $(NAMET) -lpthread -lreadline

dfc-volt: $(CORE_VOLT) $(MMGRS_VOLT) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC)
	$(CC) $(CFLAGS) $(CORE_VOLT) $(MMGRS_VOLT) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC) -o $(NAMEV) -lpthread -lreadline

clean:
	rm -f $(CLEAN) $(NAME) $(NAMET) $(NAMEV)
