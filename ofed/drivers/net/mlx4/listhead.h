#ifndef _LISTHEAD
#define _LISTHEAD
#undef LIST_HEAD
/* From sys/queue.h */
#define LIST_HEAD(name, type)                                           \
struct name {                                                           \
        struct type *lh_first;  /* first element */                     \
}

#define LIST_REMOVE_HEAD(head, field) do {                             \
        LIST_FIRST((head)) = LIST_NEXT(LIST_FIRST((head)), field);   \
} while (0)

#define LIST_FIRST(head)       ((head)->lh_first)
#define LIST_NEXT(elm, field)  ((elm)->field.le_next)

#endif
