#include <libox.h>
#include <ox-fabrics.h>
#include <ox-app.h>

static void ox_ftl_modules (void)
{
    /* Bad block table */
    oxb_bbt_byte_register ();
    /* Block meta-data */
    oxb_blk_md_register ();
    /* Channel provisioning */
    oxb_ch_prov_register ();
    /* Global provisioning */
    oxb_gl_prov_register ();
    /* Channel mapping */
    oxb_ch_map_register();
    /* Global mapping */
    oxb_gl_map_register ();
    /* Back-end PPA I/O */
    oxb_ppa_io_register ();
    /* Front-end LBA I/O */
    oxb_lba_io_register ();
    /* Garbage collection */
    oxb_gc_register ();
    /* Log Management */
    oxb_log_register ();
    /* Recovery */
    oxb_recovery_register ();
}

int main (int argc, char **argv)
{
    ox_add_mmgr (mmgr_filebe_init);

    ox_add_ftl (ftl_lnvm_init);

    ox_add_ftl (ftl_oxapp_init);
    ox_set_std_ftl (FTL_ID_OXAPP);
    ox_set_std_oxapp (FTL_ID_BLOCK);
    ox_ftl_modules ();

    ox_add_parser (parser_nvme_init);
    ox_add_parser (parser_fabrics_init);

    ox_add_transport (ox_fabrics_init);    
    ox_set_std_transport (NVM_TRANSP_FABRICS);
    ox_add_net_interface (OXF_ADDR_1, OXF_PORT_1);
    ox_add_net_interface (OXF_ADDR_2, OXF_PORT_2);

    /* We just have 2 cables for now, for the real network setup */
#if OXF_FULL_IFACES
    ox_add_net_interface (OXF_ADDR_3, OXF_PORT_3);
    ox_add_net_interface (OXF_ADDR_4, OXF_PORT_4);
#endif

    return ox_ctrl_start (argc, argv);
}
