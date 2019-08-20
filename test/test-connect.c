#include <stdio.h>
#include <libox.h>
#include <ox-fabrics.h>

int main (void)
{
    int ret;

    ret = oxf_host_init ();
    if (ret) {
        printf ("Failed to initialize Host Fabrics.\n");
        return -1;
    }

    oxf_host_add_server_iface (OXF_ADDR_1, OXF_PORT_1);

    if (oxf_host_create_queue (0)) {
        printf ("Failed to creating queue 0.\n");
        oxf_host_exit ();
        return -1;
    }

    if (oxf_host_create_queue (1)) {
        printf ("Failed to creating queue 1.\n");
        oxf_host_exit ();
        return -1;
    }

    printf ("Connection established -> %s:%d\n", OXF_ADDR_1, OXF_PORT_1);

    oxf_host_exit ();

    printf ("Connection closed      -> %s:%d\n", OXF_ADDR_1, OXF_PORT_1);

    return 0;
}
