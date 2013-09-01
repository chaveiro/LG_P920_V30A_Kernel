#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>

struct ti_prealloc {
    int occupied;
    unsigned int size;
    void * ptr;
};

void ti_prealloc_init (void);
void ti_prealloc_deinit (void);
void * ti_prealloc (unsigned int size);
int ti_prefree (void * ptr);


struct ti_prealloc ti_allocs[] = {
	{0, 8 * 1024, NULL},
	{0, 8 * 1024, NULL},
	{0, 8 * 1024, NULL},
	{0, 8 * 1024, NULL},
	{0, 16 * 1024, NULL},
	{0, 16 * 1024, NULL},
	{0, 16 * 1024, NULL},
	{0, 16 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},
	{0, 32 * 1024, NULL},	
	{0, 64 * 1024, NULL},
	{0, 64 * 1024, NULL},
};

void ti_prealloc_init (void)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(ti_allocs); i++) {
		ti_allocs[i].occupied = 0;
		ti_allocs[i].ptr = kmalloc (ti_allocs[i].size, GFP_KERNEL);
	}
}

void ti_prealloc_deinit (void)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(ti_allocs); i++) {
		kfree(ti_allocs[i].ptr);
	}
}

void * ti_prealloc (unsigned int size)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(ti_allocs); i++) {
		if (ti_allocs[i].occupied) continue;;
		if (ti_allocs[i].size > size) {
			/* we found the slot */
//			printk(KERN_INFO"%s : prealloc index%d %d -> %d\n", __func__, i, size, ti_allocs[i].size);
			ti_allocs[i].occupied = 1;
			return ti_allocs[i].ptr;
		}
	}
	printk (KERN_INFO"%s : prealloc failed\n", __func__);

	return NULL;
}
EXPORT_SYMBOL(ti_prealloc);

int ti_prefree (void * ptr)
{
	int i = 0;

	for (i = 0; i < ARRAY_SIZE(ti_allocs); i++) {
		if (ti_allocs[i].ptr == ptr) {
//			printk(KERN_INFO"%s : free index%d\n", __func__, i);	
			ti_allocs[i].occupied = 0;
			return 1;
		}
	}

	return 0;
}
EXPORT_SYMBOL(ti_prefree);



static int prekm_init(void)
{
	ti_prealloc_init();
	printk(KERN_INFO "htmsg.. start  module\n");
	return 0;
}

static void prekm_exit(void)
{
	printk(KERN_INFO "htmsg..  Exit  module\n");
}



void* pmem = NULL;
void* ti_malloc( size_t size )
{
   if (size > 4 * 1024) {
    /* try prealoc */
       pmem = ti_prealloc (size);
       if (pmem) return pmem;
   }
   return kmalloc(size, GFP_KERNEL);
}   

void ti_free( void *ptr )
{
    if (ptr == NULL)
      return;
      
    if (ti_prefree(ptr)) return;

    kfree(ptr);
}

EXPORT_SYMBOL(ti_malloc);
EXPORT_SYMBOL(ti_free);


MODULE_LICENSE("Dual BSD/GPL");
module_init(prekm_init); 
module_exit(prekm_exit); 

