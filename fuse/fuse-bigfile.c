#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <nvme-host.h>


int fuse_ox_init (void);
void fuse_ox_exit (void);
int fuse_ox_write (uint64_t slba, uint64_t size, uint8_t *buf);
int fuse_ox_read (uint64_t slba, uint64_t size, uint8_t *buf);

static uint64_t ns_size;

static int do_getattr( const char *path, struct stat *st)
{
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_atime = time( NULL );
	st->st_mtime = time( NULL );
	
	if ( strcmp( path, "/" ) == 0 )	{
		st->st_mode = S_IFDIR | 0755;
		st->st_nlink = 2;
	} else {
		st->st_mode = S_IFREG | 0664;
		st->st_nlink = 1;
		st->st_size = (off_t) ns_size * NVMEH_BLK_SZ;
	}

	return 0;
}

static int do_readdir( const char *path, void *buffer, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi )
{
	
    filler( buffer, ".", NULL, 0 ); 
    filler( buffer, "..", NULL, 0 );
	
    if ( strcmp( path, "/" ) == 0) {
	filler( buffer, "ox-full-ns", NULL, 0 );
    }
	
    return 0;
}

static int do_read( const char *path, char *buffer, size_t size, off_t offset,
		    struct fuse_file_info *fi )
{
    int ret;

    ret = fuse_ox_read ((offset / NVMEH_BLK_SZ) + 1, size, (uint8_t *) buffer);		
    return (!ret) ? size : 0;
}

static int do_opendir( const char *path, struct fuse_file_info *fi)
{
    return 0;
}

static int do_write( const char *path, const char *buffer, size_t size,
				    off_t offset, struct fuse_file_info *fi )
{
    int ret;
    uint64_t sz;
    uint8_t *rbuf;
    uint32_t offr;
   
    /* If size is not aligned, read the block, make any changes, and write it */
    if (size % NVMEH_BLK_SZ != 0) {
	rbuf = malloc (NVMEH_BLK_SZ);
	if (!rbuf)
	    return 0;

	ret = fuse_ox_read ((offset / NVMEH_BLK_SZ) + 1, NVMEH_BLK_SZ, rbuf);
	if (ret) {
	    free (rbuf);
	    return 0;
	}

	offr = offset % NVMEH_BLK_SZ;
	memcpy (&rbuf[offr], buffer, size);
    } else {
	rbuf = (uint8_t *) buffer;
    }

    sz = (((size - 1) / NVMEH_BLK_SZ) + 1) * NVMEH_BLK_SZ;

    ret = fuse_ox_write ((offset / NVMEH_BLK_SZ) + 1, sz, rbuf);		

    return (!ret) ? size : 0;
}

static int do_open( const char *path, struct fuse_file_info *fi)
{
    return 0;
}

static void *do_init( struct fuse_conn_info *conn)
{
    return NULL;
}

static struct fuse_operations operations = {
    .getattr	= do_getattr,
    .readdir	= do_readdir,
    .read	= do_read,
    .write	= do_write,
    .open	= do_open,
    .opendir	= do_opendir,
    .init	= do_init,
};

int main( int argc, char *argv[] )
{
    struct nvme_id_ns ns_id;

    fuse_ox_init ();

    if (nvmeh_identify_ns (&ns_id, 1)) {
	fuse_ox_exit ();
	return -1;
    }

    ns_size = ns_id.nsze;

    printf ("Welcome to OX BFFS (Big File File System)!\n");
    printf ("It displays a single file representing the whole OX device.\n");
    printf ("Stop this process to unmount the file system.\n");

    return fuse_main( argc, argv, &operations, NULL );
}
