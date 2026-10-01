#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libxal.h>
#include <xal.h>
#include <xal_be_fiemap.h>
#include <xal_odf.h>

#include "test.h"

#define POOL_INODES  16
#define POOL_EXTENTS 16

static struct xal *
make_xal(struct xal_sb *sb, enum xal_backend type)
{
	struct xal_backend_base *be;
	struct xal *xal;
	int err;

	xal = calloc(1, sizeof(*xal));
	if (!xal) {
		return NULL;
	}

	err = xal_pool_map(&xal->inodes, POOL_INODES, POOL_INODES, sizeof(struct xal_inode), NULL);
	if (err) {
		goto failed;
	}

	err = xal_pool_map(&xal->extents, POOL_EXTENTS, POOL_EXTENTS, sizeof(struct xal_extent),
			   NULL);
	if (err) {
		goto failed;
	}

	xal->sb = *sb;
	xal->index_state = &xal->_index_state_storage;
	xal->seq_lock = &xal->_seq_lock_storage;

	be = (struct xal_backend_base *)&xal->be;
	be->type = type;

	return xal;

failed:
	xal_close(xal);
	return NULL;
}

static int
test_inode_at(void)
{
	struct xal_sb sb = {0};
	struct xal_inode *base;
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	base = xal->inodes.memory;

	TEST_ASSERT(xal_inode_at(xal, 0) == &base[0]);
	TEST_ASSERT(xal_inode_at(xal, 1) == &base[1]);
	TEST_ASSERT(xal_inode_at(xal, 5) == &base[5]);

	xal_close(xal);
	return 0;
}

static int
test_inode_idx_roundtrip(void)
{
	struct xal_sb sb = {0};
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	TEST_ASSERT(xal_inode_idx(xal, xal_inode_at(xal, 0)) == 0);
	TEST_ASSERT(xal_inode_idx(xal, xal_inode_at(xal, 3)) == 3);
	TEST_ASSERT(xal_inode_idx(xal, xal_inode_at(xal, 7)) == 7);

	xal_close(xal);
	return 0;
}

static int
test_extent_at(void)
{
	struct xal_sb sb = {0};
	struct xal_extent *base;
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	base = xal->extents.memory;

	TEST_ASSERT(xal_extent_at(xal, 0) == &base[0]);
	TEST_ASSERT(xal_extent_at(xal, 4) == &base[4]);

	xal_close(xal);
	return 0;
}

static int
test_inode_is_dir(void)
{
	struct xal_inode inode = {0};

	inode.ftype = XAL_ODF_DIR3_FT_DIR;

	TEST_ASSERT(xal_inode_is_dir(&inode));
	TEST_ASSERT(!xal_inode_is_file(&inode));

	return 0;
}

static int
test_inode_is_file(void)
{
	struct xal_inode inode = {0};

	inode.ftype = XAL_ODF_DIR3_FT_REG_FILE;

	TEST_ASSERT(xal_inode_is_file(&inode));
	TEST_ASSERT(!xal_inode_is_dir(&inode));

	return 0;
}

static int
test_is_dirty_default_false(void)
{
	struct xal_sb sb = {0};
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);
	TEST_ASSERT(!xal_is_dirty(xal));

	xal_close(xal);
	return 0;
}

static int
test_seq_lock_default_zero(void)
{
	struct xal_sb sb = {0};
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);
	TEST_ASSERT(xal_get_seq_lock(xal) == 0);

	xal_close(xal);
	return 0;
}

static int
test_fsbno_offset_fiemap(void)
{
	struct xal_sb sb = { .blocksize = 4096 };
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	TEST_ASSERT(xal_fsbno_offset(xal, 0) == 0);
	TEST_ASSERT(xal_fsbno_offset(xal, 1) == 4096);
	TEST_ASSERT(xal_fsbno_offset(xal, 5) == 5 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_fsbno_offset_xfs_power_of_two(void)
{
	/* agblocks=8, agblklog=3: agblocks == (1 << agblklog), so
	 * the formula reduces to fsbno * blocksize. */
	struct xal_sb sb = { .blocksize = 4096, .agblocks = 8, .agblklog = 3 };
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);

	TEST_ASSERT(xal_fsbno_offset(xal, 0) == 0);
	TEST_ASSERT(xal_fsbno_offset(xal, 3) == 3 * 4096);
	TEST_ASSERT(xal_fsbno_offset(xal, 8) == 8 * 4096);
	TEST_ASSERT(xal_fsbno_offset(xal, 11) == 11 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_fsbno_offset_xfs_non_power_of_two(void)
{
	/* agblocks=6, agblklog=3: each AG holds 6 blocks but fsbno
	 * encodes the AG number in the upper bits with a stride of
	 * (1 << agblklog)=8, so AG boundaries do not coincide with
	 * simple multiples of agblocks. */
	struct xal_sb sb = { .blocksize = 4096, .agblocks = 6, .agblklog = 3 };
	struct xal *xal;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);

	/* fsbno=0: ag=0, bno=0, offset=0 */
	TEST_ASSERT(xal_fsbno_offset(xal, 0) == 0);
	/* fsbno=5: ag=0, bno=5, offset=5*4096 */
	TEST_ASSERT(xal_fsbno_offset(xal, 5) == 5 * 4096);
	/* fsbno=8: ag=1, bno=0, offset=(1*6+0)*4096=24576, not 8*4096=32768 */
	TEST_ASSERT(xal_fsbno_offset(xal, 8) == 6 * 4096);
	/* fsbno=9: ag=1, bno=1, offset=(1*6+1)*4096=28672 */
	TEST_ASSERT(xal_fsbno_offset(xal, 9) == 7 * 4096);
	/* fsbno=16: ag=2, bno=0, offset=(2*6+0)*4096=49152, not 16*4096=65536 */
	TEST_ASSERT(xal_fsbno_offset(xal, 16) == 12 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_bytes(void)
{
	struct xal_sb sb = { .blocksize = 4096 };
	struct xal_extent extent = { .start_offset = 2, .start_block = 10, .nblocks = 4 };
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_bytes(xal, &extent, &out);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(out.unit == XAL_EXTENT_UNIT_BYTES);
	TEST_ASSERT(out.start_offset == 2 * 4096);
	TEST_ASSERT(out.size == 4 * 4096);
	/* FIEMAP backend: xal_fsbno_offset = start_block * blocksize */
	TEST_ASSERT(out.start_block == 10 * 4096);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_bytes_null(void)
{
	struct xal_sb sb = { .blocksize = 4096 };
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_bytes(xal, NULL, &out);
	TEST_ASSERT(err == -EINVAL);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_lba(void)
{
	struct xal_sb sb = { .blocksize = 4096, .lba_blksze = 512 };
	struct xal_extent extent = { .start_offset = 2, .start_block = 10, .nblocks = 4 };
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_lba(xal, &extent, &out);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(out.unit == XAL_EXTENT_UNIT_LBA);
	TEST_ASSERT(out.start_offset == (2 * 4096) / 512);
	TEST_ASSERT(out.size == (4 * 4096) / 512);
	TEST_ASSERT(out.start_block == (10 * 4096) / 512);

	xal_close(xal);
	return 0;
}

static int
test_extent_in_lba_null(void)
{
	struct xal_sb sb = { .blocksize = 4096, .lba_blksze = 512 };
	struct xal_extent_converted out = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	err = xal_extent_in_lba(xal, NULL, &out);
	TEST_ASSERT(err == -EINVAL);

	xal_close(xal);
	return 0;
}

static int
count_visits_cb(struct xal *xal, struct xal_inode *inode, void *cb_args, int level)
{
	int *count = cb_args;

	(void)xal;
	(void)inode;
	(void)level;

	(*count)++;
	return 0;
}

static int
test_walk_visits_all(void)
{
	struct xal_sb sb = {0};
	struct xal_inode *inodes;
	struct xal *xal;
	int count;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	inodes = xal_inode_at(xal, 0);

	/* root: dir with 2 children starting at index 1 */
	inodes[0].ftype = XAL_ODF_DIR3_FT_DIR;
	inodes[0].content.dentries.inodes_idx = 1;
	inodes[0].content.dentries.count = 2;
	/* child 0: regular file */
	inodes[1].ftype = XAL_ODF_DIR3_FT_REG_FILE;
	/* child 1: subdir with 1 child at index 3 */
	inodes[2].ftype = XAL_ODF_DIR3_FT_DIR;
	inodes[2].content.dentries.inodes_idx = 3;
	inodes[2].content.dentries.count = 1;
	/* grandchild: regular file */
	inodes[3].ftype = XAL_ODF_DIR3_FT_REG_FILE;

	count = 0;
	err = xal_walk(xal, xal_inode_at(xal, 0), count_visits_cb, &count);
	TEST_ASSERT(err == 0);
	TEST_ASSERT(count == 4);

	xal_close(xal);
	return 0;
}

static int
test_walk_dirty_returns_estale(void)
{
	struct xal_sb sb = {0};
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);

	xal_mark_dirty(xal);

	err = xal_walk(xal, xal_inode_at(xal, 0), NULL, NULL);
	TEST_ASSERT(err == -ESTALE);

	xal_close(xal);
	return 0;
}

static void
set_entry(struct xal_inode *inode, const char *name, uint8_t ftype, uint32_t parent_idx)
{
	inode->ftype = ftype;
	inode->namelen = strlen(name);
	memcpy(inode->name, name, inode->namelen + 1);
	inode->parent_idx = parent_idx;
}

// Index 0 is the root, 1 is the directory "dir" and 2 is the file "file" inside it
static void
build_path_tree(struct xal *xal)
{
	set_entry(xal_inode_at(xal, 0), "", XAL_ODF_DIR3_FT_DIR, XAL_POOL_IDX_NONE);
	set_entry(xal_inode_at(xal, 1), "dir", XAL_ODF_DIR3_FT_DIR, 0);
	set_entry(xal_inode_at(xal, 2), "file", XAL_ODF_DIR3_FT_REG_FILE, 1);
}

static int
test_inode_path_xfs(void)
{
	struct xal_sb sb = {0};
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, sizeof(buf));
	TEST_ASSERT(err == (int)strlen("/dir/file"));
	TEST_ASSERT(strcmp(buf, "/dir/file") == 0);

	err = xal_inode_path(xal, xal_inode_at(xal, 1), buf, sizeof(buf));
	TEST_ASSERT(err == (int)strlen("/dir"));
	TEST_ASSERT(strcmp(buf, "/dir") == 0);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_xfs_root(void)
{
	struct xal_sb sb = {0};
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	err = xal_inode_path(xal, xal_inode_at(xal, 0), buf, sizeof(buf));
	TEST_ASSERT(err == 1);
	TEST_ASSERT(strcmp(buf, "/") == 0);

	err = xal_inode_path(xal, xal_inode_at(xal, 0), buf, 1);
	TEST_ASSERT(err == -ENAMETOOLONG);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_fiemap_mountpoint(void)
{
	struct xal_sb sb = {0};
	struct xal_be_fiemap *be;
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	be = (struct xal_be_fiemap *)&xal->be;
	be->mountpoint = "/mnt/xal";

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, sizeof(buf));
	TEST_ASSERT(err == (int)strlen("/mnt/xal/dir/file"));
	TEST_ASSERT(strcmp(buf, "/mnt/xal/dir/file") == 0);

	err = xal_inode_path(xal, xal_inode_at(xal, 0), buf, sizeof(buf));
	TEST_ASSERT(err == (int)strlen("/mnt/xal"));
	TEST_ASSERT(strcmp(buf, "/mnt/xal") == 0);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_fiemap_subtree(void)
{
	struct xal_sb sb = {0};
	struct xal_be_fiemap *be;
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	be = (struct xal_be_fiemap *)&xal->be;
	be->mountpoint = "/mnt/xal";
	be->subtree = "/mnt/xal/sub";

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, sizeof(buf));
	TEST_ASSERT(err == (int)strlen("/mnt/xal/sub/dir/file"));
	TEST_ASSERT(strcmp(buf, "/mnt/xal/sub/dir/file") == 0);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_buffer_boundary(void)
{
	struct xal_sb sb = {0};
	char buf[64];
	size_t nbytes;
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	nbytes = strlen("/dir/file") + 1;

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, nbytes);
	TEST_ASSERT(err == (int)nbytes - 1);
	TEST_ASSERT(strcmp(buf, "/dir/file") == 0);

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, nbytes - 1);
	TEST_ASSERT(err == -ENAMETOOLONG);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_basepath_too_long(void)
{
	struct xal_sb sb = {0};
	struct xal_be_fiemap *be;
	char buf[8];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_FIEMAP);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	be = (struct xal_be_fiemap *)&xal->be;
	be->mountpoint = "/mnt/xal";

	err = xal_inode_path(xal, xal_inode_at(xal, 0), buf, sizeof(buf));
	TEST_ASSERT(err == -ENAMETOOLONG);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_invalid_args(void)
{
	struct xal_sb sb = {0};
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	err = xal_inode_path(NULL, xal_inode_at(xal, 2), buf, sizeof(buf));
	TEST_ASSERT(err == -EINVAL);

	err = xal_inode_path(xal, NULL, buf, sizeof(buf));
	TEST_ASSERT(err == -EINVAL);

	err = xal_inode_path(xal, xal_inode_at(xal, 2), NULL, sizeof(buf));
	TEST_ASSERT(err == -EINVAL);

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, 0);
	TEST_ASSERT(err == -EINVAL);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_malformed_parent(void)
{
	struct xal_sb sb = {0};
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	// A parent at or after its child would loop; it must be refused
	xal_inode_at(xal, 1)->parent_idx = 2;

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, sizeof(buf));
	TEST_ASSERT(err == -EINVAL);

	xal_inode_at(xal, 1)->parent_idx = 1;

	err = xal_inode_path(xal, xal_inode_at(xal, 1), buf, sizeof(buf));
	TEST_ASSERT(err == -EINVAL);

	xal_close(xal);
	return 0;
}

static int
test_inode_path_dirty_returns_estale(void)
{
	struct xal_sb sb = {0};
	char buf[64];
	struct xal *xal;
	int err;

	xal = make_xal(&sb, XAL_BACKEND_XFS);
	TEST_ASSERT(xal != NULL);
	build_path_tree(xal);

	xal_mark_dirty(xal);

	err = xal_inode_path(xal, xal_inode_at(xal, 2), buf, sizeof(buf));
	TEST_ASSERT(err == -ESTALE);

	xal_close(xal);
	return 0;
}

int
main(void)
{
	int failures = 0;

	TEST_RUN(test_inode_at);
	TEST_RUN(test_inode_idx_roundtrip);
	TEST_RUN(test_extent_at);
	TEST_RUN(test_inode_is_dir);
	TEST_RUN(test_inode_is_file);
	TEST_RUN(test_is_dirty_default_false);
	TEST_RUN(test_seq_lock_default_zero);
	TEST_RUN(test_fsbno_offset_fiemap);
	TEST_RUN(test_fsbno_offset_xfs_power_of_two);
	TEST_RUN(test_fsbno_offset_xfs_non_power_of_two);
	TEST_RUN(test_extent_in_bytes);
	TEST_RUN(test_extent_in_bytes_null);
	TEST_RUN(test_extent_in_lba);
	TEST_RUN(test_extent_in_lba_null);
	TEST_RUN(test_walk_visits_all);
	TEST_RUN(test_walk_dirty_returns_estale);
	TEST_RUN(test_inode_path_xfs);
	TEST_RUN(test_inode_path_xfs_root);
	TEST_RUN(test_inode_path_fiemap_mountpoint);
	TEST_RUN(test_inode_path_fiemap_subtree);
	TEST_RUN(test_inode_path_buffer_boundary);
	TEST_RUN(test_inode_path_basepath_too_long);
	TEST_RUN(test_inode_path_invalid_args);
	TEST_RUN(test_inode_path_malformed_parent);
	TEST_RUN(test_inode_path_dirty_returns_estale);

	return failures;
}
