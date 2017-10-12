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
# (2) Macro to define global Open-Channel geometry (in bits)
# ox-ctrl and ox-ctrl-test
CONFIG_DFC  = -DCONFIG_MMGR_DFCNAND	    # (1)
CONFIG_DFC += $(CONFIG_FTL)		    # (1)
CONFIG_DFC += -DCONFIG_NVM_SECSZ=0x1000     # (2)
CONFIG_DFC += -DCONFIG_NVM_SEC_OOBSZ=0x10   # (2)
CONFIG_DFC += -DCONFIG_NVM_SEC_PG=2	    # (2)
CONFIG_DFC += -DCONFIG_NVM_PG_BLK=9	    # (2)
CONFIG_DFC += -DCONFIG_NVM_CH=3             # (2)
CONFIG_DFC += -DCONFIG_NVM_LUN_CH=2	    # (2)
CONFIG_DFC += -DCONFIG_NVM_BLK_LUN=10	    # (2)
CONFIG_DFC += -DCONFIG_NVM_PLANES=1	    # (2)
CONFIG_DFC += -DCONFIG_NVM_RESERVED=37	    # (2)
# ox-ctrl-volt
CONFIG_VOLT  = -DCONFIG_MMGR_VOLT		# (1)
CONFIG_VOLT += $(CONFIG_FTL)			# (1)
CONFIG_VOLT += -DCONFIG_NVM_SECSZ=0x1000	# (2)
CONFIG_VOLT += -DCONFIG_NVM_SEC_OOBSZ=0x10	# (2)
CONFIG_VOLT += -DCONFIG_NVM_SEC_PG=2		# (2)
CONFIG_VOLT += -DCONFIG_NVM_PG_BLK=6		# (2)
CONFIG_VOLT += -DCONFIG_NVM_CH=3		# (2)
CONFIG_VOLT += -DCONFIG_NVM_LUN_CH=2		# (2)
CONFIG_VOLT += -DCONFIG_NVM_BLK_LUN=5		# (2)
CONFIG_VOLT += -DCONFIG_NVM_PLANES=1		# (2)
CONFIG_VOLT += -DCONFIG_NVM_RESERVED=45         # (2)

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
LNVM_FTL_VOLT = $(LNVM_FTL_PATH)/ftl_lnvm-v.o $(LNVM_FTL_PATH)/lnvm_bbtbl-v.o
CLEAN += $(LNVM_FTL_PATH)/*.o $(LNVM_FTL_PATH)/*-v.o
#Join flash translation layers in FTLS, only LNVM_FTL for now
FTLS_DFC = $(LNVM_FTL)
FTLS_VOLT = $(LNVM_FTL_VOLT)

### INTERCONNECT HANDLERS
PCIE_DFC_PATH = pcie_dfc
PCIE_DFC = $(PCIE_DFC_PATH)/pcie_dfc.o
PCIE_VOLT = $(PCIE_DFC_PATH)/pcie_dfc-v.o
CLEAN += $(PCIE_DFC_PATH)/*.o $(PCIE_DFC_PATH)/*-v.o

### TESTS
TESTS_DFC_PATH = test
TESTS_DFC = $(TESTS_DFC_PATH)/test_core.o $(TESTS_DFC_PATH)/test_mmgr.o
TESTS_DFC += $(TESTS_DFC_PATH)/test_lightnvm.o $(TESTS_DFC_PATH)/test_admin.o
TESTS_VOLT = $(TESTS_DFC_PATH)/test_core-v.o $(TESTS_DFC_PATH)/test_mmgr-v.o
TESTS_VOLT += $(TESTS_DFC_PATH)/test_lightnvm-v.o $(TESTS_DFC_PATH)/test_admin-v.o
CLEAN += $(TESTS_DFC_PATH)/*.o $(TESTS_DFC_PATH)/*-v.o

### GLOBAL FLAGS
#CC = #For yocto project CC comes from Yocto Makefile
CFLAGS = -g -lrt
CFLAGSXX = -pg -fPIC -shared -c -o
DEPS = include/ssd.h include/nvme.h include/lightnvm.h include/ox-mq.h

### CORE
# ox-ctrl and ox-ctrl-test
%.o: %.c $(DEPS)
	$(CC) $(CONFIG_DFC) $(CFLAGSXX) $@ $< $(CFLAGS)
# ox-ctrl-volt
%-v.o: %.c $(DEPS)
	$(CC) $(CONFIG_VOLT) $(CFLAGSXX) $@ $< $(CFLAGS)

### DFC NAND MEDIA MANAGER
$(DFCNAND_PATH)/%.o : $(DFCNAND_PATH)/%.c $(DEPS)
	$(CC) $(CONFIG_DFC) $(CFLAGSXX) $@ $< $(CFLAGS) $(DFCNAND_PATH)/nand_dm.a

### VOLT MEDIA MANAGER
$(VOLT_PATH)/%.o : $(VOLT_PATH)/%.c $(DEPS)
	$(CC) $(CONFIG_VOLT) $(CFLAGSXX) $@ $< $(CFLAGS)

### LNVM FTL
$(LNVM_FTL_PATH)/%.o : $(LNVM_FTL_PATH)/%.c $(DEPS)
	$(CC) $(CONFIG_DFC) $(CFLAGSXX) $@ $< $(CFLAGS)

### LNVM FTL VOLT
$(LNVM_FTL_PATH)/%-v.o : $(LNVM_FTL_PATH)/%.c $(DEPS)
	$(CC) $(CONFIG_VOLT) $(CFLAGSXX) $@ $< $(CFLAGS)

### DFC PCIe INTERCONNECT
$(PCIE_DFC_PATH)/%.o : %.c $(DEPS)
	$(CC) $(CONFIG_DFC) $(CFLAGSXX) $@ $< $(CFLAGS)

### DFC PCIe INTERCONNECT VOLT
$(PCIE_DFC_PATH)/%-v.o : %.c $(DEPS)
	$(CC) $(CONFIG_VOLT) $(CFLAGSXX) $@ $< $(CFLAGS)

### DFC TESTS
$(TESTS_DFC_PATH)/%.o : %.c include/tests.h
	$(CC) $(CONFIG_DFC) $(CFLAGSXX) $@ $< $(CFLAGS) -Wall

### DFC TESTS VOLT
$(TESTS_DFC_PATH)/%-v.o : %.c include/tests.h
	$(CC) $(CONFIG_VOLT) $(CFLAGSXX) $@ $< $(CFLAGS) -Wall

.PHONY: all clean

### TARGETS ###
all: dfc dfc-tests dfc-volt

dfc: $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC)
	$(CC) $(CFLAGS) $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) -o $(NAME) -lpthread -lreadline

dfc-tests: $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC)
	$(CC) $(CFLAGS) $(CORE) $(MMGRS_DFC) $(FTLS_DFC) $(PCIE_DFC) $(TESTS_DFC) -o $(NAMET) -lpthread -lreadline

dfc-volt: $(CORE_VOLT) $(MMGRS_VOLT) $(FTLS_VOLT) $(PCIE_VOLT) $(TESTS_VOLT)
	$(CC) $(CFLAGS) $(CORE_VOLT) $(MMGRS_VOLT) $(FTLS_VOLT) $(PCIE_VOLT) $(TESTS_VOLT) -o $(NAMEV) -lpthread -lreadline

clean:
	rm -f $(CLEAN) $(NAME) $(NAMET) $(NAMEV)
