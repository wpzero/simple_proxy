#include "sblist.h"
#include <string.h>

/* 这个delete需要把item之后的所有的数据进行移位 */
void sblist_delete(sblist* l, size_t item) {
        if (l->count && item < l->count) {
                memmove(sblist_item_from_index(l, item), sblist_item_from_index(l, item + 1), (sblist_getsize(l) - (item + 1)) * l->itemsize);
                l->count--;
        }
}
