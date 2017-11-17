#include <openenclave/bits/heap.h>
#include <openenclave/thread.h>
#include <openenclave/bits/search.h>

#ifdef OE_BUILD_UNTRUSTED
# include <stdio.h>
# define U(X) X
#else
# define U(X)
#endif

/* Initialize the heap structure. Caller acquires the lock */
int OE_HeapInit(
    OE_Heap* heap,
    uintptr_t base,
    size_t size)
{
    int rc = -1;

    /* Check bad parameters */
    if (!heap || !base || !size)
        goto done;

    /* BASE must be aligned on a page boundary */
    if (base % OE_PAGE_SIZE)
        goto done;

    /* SIZE must be a mulitple of the page size */
    if (size % OE_PAGE_SIZE)
        goto done;

    /* Calculate the total number of pages */
    size_t num_pages = size / OE_PAGE_SIZE;

    /* Save the base of the heap */
    heap->base = base;

    /* Save the size of the heap */
    heap->size = size;

    /* Set the start of the heap area, which follows the VADs array */
    heap->start = base + (num_pages * sizeof(OE_VAD));

    /* Set the end of the heap area */
    heap->end = base + size;

    /* Set the top of the break memory (grows positively) */
    heap->break_top = heap->start;

    /* Set the top of the mapped memory (grows negativey) */
    heap->mapped_top = heap->end;

    /* Set pointer to the next available entry in the OE_VAD array */
    heap->next_vad = (OE_VAD*)base;

    /* Set pointer to the end address of the OE_VAD array */
    heap->end_vad = (OE_VAD*)heap->start;

    /* Set the free OE_VAD list to null */
    heap->free_vads = NULL;

    /* Set the root of the OE_VAD tree to null */
    heap->vad_tree = NULL;

    /* Set the OE_VAD linked list to null */
    heap->vad_list = NULL;

    /* Initialize the lock field */
    {
        OE_Spinlock dummy = OE_SPINLOCK_INITIALIZER;
        heap->lock = dummy;
    }

    /* Finally, set initialized to true */
    heap->initialized = 1;

    rc = 0;

done:
    return rc;
}

/* Get a free OE_VAD */
static OE_VAD* _GetVAD(
    OE_Heap* heap)
{
    OE_VAD* vad = NULL;

    /* First try the free list */
    if (heap->free_vads)
    {
        vad = heap->free_vads;
        heap->free_vads = vad->next;
        goto done;
    }

    /* Now try the OE_VAD array */
    if (heap->next_vad != heap->end_vad)
    {
        vad = heap->next_vad++;
        goto done;
    }

done:
    return vad;
}

/* Return a free OE_VAD */
OE_INLINE void _PutVAD(
    OE_Heap* heap,
    OE_VAD* vad)
{
    /* Clear the VAD */
    vad->addr = 0;
    vad->size = 0;
    vad->prot = 0;
    vad->flags = 0;

    /* Insert into singly-linked free list as first element */
    vad->next = heap->free_vads;
    heap->free_vads = vad;
}

static OE_VAD* _NewVAD(
    OE_Heap* heap,
    uintptr_t addr, 
    size_t size,
    int prot,
    int flags)
{
    OE_VAD* vad;

    if (!(vad = _GetVAD(heap)))
        return NULL;

    vad->addr = addr;
    vad->size = (uint32_t)size;
    vad->prot = (uint16_t)prot;
    vad->flags = (uint16_t)flags;

    return vad;
}

/* Comparison function to compare to VADs for equality */
static int _Compare(const void *lhsp, const void *rhsp)
{
    OE_VAD* lhs = (OE_VAD*)lhsp;
    OE_VAD* rhs = (OE_VAD*)rhsp;

    if (lhs->addr < rhs->addr)
        return -1;

    if (lhs->addr > rhs->addr)
        return 1;

    return 0;
}

/* Comparison function for finding VAD that contains an address */
static int _RangeCompare(const void *keyp, const void *vadp)
{
    uintptr_t key = *(uintptr_t*)keyp;
    OE_VAD* vad = (OE_VAD*)vadp;

    uint64_t lo = vad->addr;
    uint64_t hi = vad->addr + vad->size;

    if (key >= lo && key < hi)
        return 0;

    return key < lo ? -1 : 1;
}

static void* _Alloc(size_t size, void* data)
{
    /* data is a OE_VAD pointer */
    return data;
}

/* Insert OE_VAD into tree */
static int _TreeInsert(
    OE_Heap* heap,
    OE_VAD* vad)
{
    int rc = -1;
    void* ret;

    vad->tnode.key = vad;
    ret = OE_Tsearch(vad, (void**)&heap->vad_tree, _Compare, _Alloc, vad);

    if (ret != vad)
        goto done;

    rc = 0;

done:
    return rc;
}

OE_INLINE bool _IsSorted(OE_VAD* list)
{
    OE_VAD* p;
    OE_VAD* prev = NULL;

    for (p = list; p; p = p->next)
    {
        if (prev && !(prev->addr < p->addr))
            return false;

        prev = p;
    }

    return true;
}

/* TODO: optimize by using tree to find the insertion point in O(log n) */
static void _ListInsert(
    OE_Heap* heap,
    OE_VAD* vad)
{
    /* If the list is empty */
    if (!heap->vad_list)
    {
        heap->vad_list = vad;
        vad->prev = NULL;
        vad->next = NULL;
        return;
    }

    /* Insert into list sorted by address */
    {
        OE_VAD* p;
        OE_VAD* prev = NULL;

        /* Find element prev, such that prev->addr < vad->addr */
        for (p = heap->vad_list; p && p->addr < vad->addr; p = p->next)
            prev = p;

        /* Insert after 'prev' if non-null, else insert at head */
        if (prev)
        {
            vad->next = prev->next;

            if (prev->next)
                prev->next->prev = vad;

            prev->next = vad;
            vad->prev = prev;
        }
        else
        {
            vad->next = heap->vad_list;
            vad->prev = NULL;

            if (heap->vad_list)
                heap->vad_list->prev = vad;

            heap->vad_list = vad;
        }
    }
}

static int _Insert(
    OE_Heap* heap,
    OE_VAD* vad)
{
    if (_TreeInsert(heap, vad) != 0)
        return -1;

    _ListInsert(heap, vad);

    return 0;
}

/* TODO: optimize by adding a 'gap' field in the tree to find gaps O(log n) */
static uintptr_t _FindRegion(
    OE_Heap* heap,
    size_t size)
{
    uintptr_t addr = 0;

    /* Search for a gap in the linked list */
    {
        OE_VAD* p;
        OE_VAD* prev = NULL;
            
        /* Visit every element in the linked list */
        for (p = heap->vad_list; p; p = p->next)
        {
            uintptr_t start;
            uintptr_t end;

            if (prev)
            {
                /* Looking for gap between current and previous element */
                start = prev->addr + prev->size;
                end = p->addr;
            }
            else
            {
                /* Looking for gap between head element and mapped top */
                start = heap->mapped_top;
                end = p->addr;
            }

            /* If the gap is big enough */
            if (end - start >= size)
            {
                addr = start;
U( printf("CASE1\n"); )
                goto done;
            }

            prev = p;
        }

        if (prev)
        {
            /* Looking for gap between last element and end of heap */
            uintptr_t start = prev->addr + prev->size;
            uintptr_t end = heap->end;

            /* If the gap is big enough */
            if (end - start >= size)
            {
                addr = start;
U( printf("CASE2\n"); )
                goto done;
            }
        }
    }

    /* No gaps in linked list so obtain memory from mapped memory area */
    {
        uintptr_t start = heap->mapped_top - size;

        /* If memory was exceeded (overrun of break top) */
        if (start < heap->break_top)
            goto done;

U( printf("CASE3\n"); )
        heap->mapped_top = start;
        addr = start;
    }

done:
    return addr;
}

static OE_VAD* _FindVAD(
    OE_Heap* heap,
    uintptr_t addr)
{
    return (OE_VAD*)OE_Tfind(&addr, (void**)&heap->vad_tree, _RangeCompare);
}

static int _RemoveVAD(OE_Heap* heap, OE_VAD* vad)
{
    int rc = -1;

    /* Check parameters */
    if (!heap || !vad || !heap->vad_list || !heap->vad_tree)
        goto done;

    /* Remove from tree */
    {
        void* ret;
        
        if (!(ret = OE_Tdelete(vad, (void**)&heap->vad_tree, _Compare, NULL)))
            goto done;
    }

    /* Remove from doubly-linked list */
    if (vad == heap->vad_list)
    {
        heap->vad_list = vad->next;

        if (vad->next)
            vad->next->prev = NULL;
    }
    else
    {
        if (vad->prev)
            vad->prev->next = vad->next;

        if (vad->next)
            vad->next->prev = vad->prev;
    }

    /* If the linked list is empty, reset the mapped top */
    if (heap->vad_list == NULL)
        heap->mapped_top = heap->end;

    rc = 0;

done:
    return rc;
}

void* OE_HeapMap(
    OE_Heap* heap,
    void* address,
    size_t size,
    int prot,
    int flags)
{
    void* result = NULL;
    uintptr_t start = 0;

    /* ATTN: check prot and flags! */

    /* Round size to multiple of the page size */
    size = (size + OE_PAGE_SIZE - 1) / OE_PAGE_SIZE * OE_PAGE_SIZE;

    if (address)
    {
        /* ATTN: implement */
        goto done;
    }
    else
    {
        if (!(start = _FindRegion(heap, size)))
            goto done;
    }

    /* Create OE_VAD for this region and inject it into the tree and list */
    {
        OE_VAD* vad;

        /* Allocate a OE_VAD for this new region */
        if (!(vad = _NewVAD(heap, start, size, prot, flags)))
            goto done;

        if (_Insert(heap, vad) != 0)
            goto done;
    }

    result = (void*)start;

done:
    return result;
}

int OE_HeapUnmap(
    OE_Heap* heap,
    void* address,
    size_t size)
{
    int rc = -1;
    OE_VAD* vad;

    if (!heap || !address || !size)
        goto done;

    /* ADDRESS must be aligned on a page boundary */
    if ((uintptr_t)address % OE_PAGE_SIZE)
        goto done;

    /* SIZE must be a multiple of the page size */
    if (size % OE_PAGE_SIZE)
        goto done;

    /* Find the VAD that contains this address */
    if (!(vad = _FindVAD(heap, (uintptr_t)address)))
        goto done;

    /* Remove the VAD from the tree and list */
    if (_RemoveVAD(heap, vad) != 0)
        goto done;

    /* ATTN: handle overlapping VADs */

    /* Return the VAD to the free list */
    _PutVAD(heap, vad);

    rc = 0;

done:
    return rc;
}
