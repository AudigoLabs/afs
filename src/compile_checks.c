#include "afs/afs.h"
#include "impl_types.h"
#include "storage_types.h"

// Make sure all the impl types fit within the corresponding private buffers
_Static_assert(sizeof(afs_impl_t) == sizeof(((afs_handle_def_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(afs_obj_impl_t) == sizeof(((afs_object_handle_def_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(afs_read_pos_impl_t) == sizeof(((afs_read_position_t*)0)->priv), "Invalid private buffer size");
_Static_assert(sizeof(afs_object_list_entry_impl_t) == sizeof(((afs_object_list_entry_t*)0)->priv), "Invalid private buffer size");

// Make sure our offset array buffer sizes match
_Static_assert(sizeof(((afs_read_pos_impl_t*)0)->object_offset) == sizeof(((afs_obj_impl_t*)0)->object_offset), "Invalid object_offset sizes");
_Static_assert(sizeof(((afs_read_pos_impl_t*)0)->block_offset) == sizeof(((afs_obj_impl_t*)0)->block_offset), "Invalid block_offset sizes");

// Make sure the footer fits within the allocated space
_Static_assert(sizeof(block_footer_t) + sizeof(chunk_header_t) + AFS_NUM_STREAMS * sizeof(uint32_t) <= BLOCK_FOOTER_LENGTH, "Overflowing footer space");
