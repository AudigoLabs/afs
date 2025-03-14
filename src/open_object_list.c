#include "open_object_list.h"

#include "afs_config.h"
#include "lookup_table.h"
#include "storage_types.h"

#define FOREACH_OPEN_OBJECT(AFS, VAR) \
    for (afs_obj_impl_t* VAR = (AFS)->open_object_list_head; VAR; VAR = (VAR)->next_open_object)

#define FOREACH_OPEN_OBJECT_CONST(AFS, VAR) \
    for (const afs_obj_impl_t* VAR = (AFS)->open_object_list_head; VAR; VAR = (VAR)->next_open_object)

//! Adds an open object to the list
void open_object_list_add(afs_impl_t* afs, afs_obj_impl_t* obj) {
    AFS_ASSERT_EQ(obj->next_open_object, NULL);
    // Add to the head of the list since that's easiest
    obj->next_open_object = afs->open_object_list_head;
    afs->open_object_list_head = obj;
}

//! Removes an open object from the list
void open_object_list_remove(afs_impl_t* afs, afs_obj_impl_t* obj) {
    afs_obj_impl_t* prev = NULL;
    FOREACH_OPEN_OBJECT(afs, cur) {
        if (cur == obj) {
            if (prev) {
                prev->next_open_object = obj->next_open_object;
            } else {
                afs->open_object_list_head = obj->next_open_object;
            }
            obj->next_open_object = NULL;
            return;
        }
        prev = cur;
    }
    AFS_FAIL("Did not find object in list");
}

bool open_object_list_contains(const afs_impl_t* afs, uint16_t object_id) {
    FOREACH_OPEN_OBJECT_CONST(afs, open_obj) {
        if (open_obj->object_id == object_id) {
            return true;
        }
    }
    return false;
}

bool open_object_list_is_empty(const afs_impl_t* afs) {
    return !afs->open_object_list_head;
}

uint16_t open_object_list_get_writing_no_storage(afs_impl_t* afs, uint16_t prev_index) {
    // Check the objects which are open for writing and haven't written to the storage yet
    uint16_t open_index = 0;
    FOREACH_OPEN_OBJECT_CONST(afs, open_obj) {
        if (open_obj->state != OBJ_STATE_WRITING) {
            continue;
        } else if (lookup_table_get_num_blocks(&afs->lookup_table, open_obj->object_id)) {
            // This object has been written to storage
            continue;
        } else if (open_index++ < prev_index) {
            continue;
        }
        return open_obj->object_id;
    }
    // No more entries
    return INVALID_OBJECT_ID;
}
