#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/kernel.h>

#include <trace/events/mlcache.h>
#include "mlcache.h"

#define PROCNAME ("mlcache")
#define MLCACHE_SCALE (100) /* the learning model's scaling factor */

static unsigned long hits;
static unsigned long misses;

#ifdef CONFIG_MLCACHE_ACTIVE
static unsigned long t;
static long weight_average = 0;
static long items_in_cache = 0;
#endif

long get_mlcache_weighted_average()
{
#ifdef CONFIG_MLCACHE_ACTIVE
	return weight_average;
#else
	return 0;
#endif
}

#ifdef CONFIG_MLCACHE_ACTIVE
static unsigned long upperBound(int step, int numPlays) {
		//indexing from 0
		if (step !=0 && numPlays != 0) {
				return int_sqrt(MLCACHE_SCALE * MLCACHE_SCALE * 2 * ilog2(MLCACHE_SCALE * MLCACHE_SCALE * (step+1)) / numPlays);
		}
		return 0;
}


static void update_page_score(struct page *page, long by, bool hit) {
		if (!page->mapping)
				return;
		if (hit) {
				page->mlcache_score -= by;
				page->mlcache_score = page->mlcache_score + upperBound(t-1, page->mlcache_plays) - upperBound(t, page->mlcache_plays)*page->mlcache_plays;
		} else
		{
				if(weight_average == UINT_MAX)
					weight_average = 0;	
				page->mlcache_score = weight_average;
				/* else there was a shadow entry found */
		}
}

static void update_average(struct page *page)
{
	if(items_in_cache == 0)
		items_in_cache = (unsigned long) ~0;

	/* change this to add the average score */
	weight_average = weight_average + (page->mlcache_score/items_in_cache);
}

static void penalize_pages(struct page *page, struct address_space *mapping)
{
		void **slot;
		struct radix_tree_iter iter;
		unsigned int checked = 0;


		if(!mapping)
			return;

		rcu_read_lock();
		/* for a miss, we search the entire radix tree and 
		 * find the shadow entry. 
		 **/
		radix_tree_for_each_slot(slot, &mapping->page_tree, &iter, 0) {
			struct page *p;

			p = radix_tree_deref_slot(slot);
			if (unlikely(!p))
				continue;

			if (radix_tree_exception(p)) {
					if (radix_tree_deref_retry(p))
							slot = radix_tree_iter_retry(&iter);

					continue;
			}

			if (p->mapping != NULL)
				continue;

			if (p == page)
				continue;
						

		    update_page_score(p, -MLCACHE_SCALE, 0);

			checked++;
			if ((checked % 4096) != 0)
				continue;

			/* goto the next slot. This one is over */
			slot = radix_tree_iter_resume(slot, &iter);
			cond_resched_rcu();
		}

		rcu_read_unlock();
}

static void update_cache_scores(struct page *page, struct address_space *mapping, bool hit)
{
		void **slot;
		struct radix_tree_iter iter;
		unsigned int checked = 0;
		unsigned char penalize = 0;

		if (hit) {
			/* we do not reduce the value of the other pages */
				update_page_score(page, MLCACHE_SCALE, hit);
				return;
		}

		if(!mapping)
			return;

		rcu_read_lock();
		/* for a miss, we search the entire radix tree and 
		 * find the shadow entry. 
		 **/
		radix_tree_for_each_slot(slot, &mapping->page_tree, &iter, 0) {
			struct page *p;

			p = radix_tree_deref_slot(slot);
			if (unlikely(!p))
				continue;

			if (radix_tree_exception(p)) {
					if (radix_tree_deref_retry(p))
							slot = radix_tree_iter_retry(&iter);

					continue;
			}

			if (p->mapping != NULL)
				continue;

			if (p == page)  {
					/* we want to increase the reward of the missed page 
					 * if there is a shadow entry we use that, else
					 * we use the average from the other entries.
					 */

					if(p->mlcache_score == 0) {
						/* no shadow entry was found. So we do not
						 * reduce the average weight of the other 
						 * pages
						 */
						update_page_score(p, 0, 0);
						break;
					} else {
						/* a shadow entry was found. So this page was 
						 * wrongly evicted by the other pages. So we
						 * penalize the other pages
						 */
						penalize = 1;
						break;
					}
						
			}

		    update_page_score(p, -MLCACHE_SCALE, 0);

			checked++;
			if ((checked % 4096) != 0)
				continue;

			/* goto the next slot. This one is over */
			slot = radix_tree_iter_resume(slot, &iter);
			cond_resched_rcu();
		}

		rcu_read_unlock();
		if (penalize) {
			penalize_pages(page, mapping);
		}
}
#endif

/* this is called from three places:
 * 1) generic_file_buffered_read()
 * 2) generic_perform_write()
 * The score is read in from a shadow page when the page
 * is added to LRU and if there was a shadow.
 * In this case, we start off with the shadow value
 * and increase the value for a hit
 * Eviction is done in pack_shadow()
 */

static void mlcache_pageget(void *data, struct page *page, struct address_space *mapping, bool hit)
{
		if (page == NULL)
				return;

		if (hit)
			hits++;
		else
			misses++;

#ifdef CONFIG_MLCACHE_ACTIVE
			t++;
			update_cache_scores(page, mapping, hit);
			items_in_cache++;
			update_average(page);
#endif
}

static int mlcache_hits_show(struct seq_file *m, void *v) {
		seq_printf(m, "Hits: %ld | Misses: %ld\n", hits, misses);
		return 0;
}

static int mlcache_hits_open(struct inode *inode, struct	file *filp) {
		return single_open(filp, mlcache_hits_show, PDE_DATA(inode));
}

static ssize_t mlcache_hits_write(struct file * m, const char *buf, size_t len, loff_t *off) {
		return len;
}



static const struct file_operations filter_fops = {
		.owner = THIS_MODULE,
		.open = mlcache_hits_open,
		.read = seq_read,
		.write = mlcache_hits_write,
		.llseek = seq_lseek,
		.release = single_release,
};

static int __init mlcache_init(void)
{
		struct proc_dir_entry *entry;

		entry = proc_create("mlcache_stats", 0444, NULL, &filter_fops);
		if (!entry)
			return NULL;

		hits = misses = 0;
#ifdef CONFIG_MLCACHE_ACTIVE
		t = 0;
#endif
		register_trace_mlcache_event(mlcache_pageget, NULL);
		return 0;
}

static void __exit mlcache_exit(void)
{
	remove_proc_entry("mlcache_stats", NULL);
	unregister_trace_mlcache_event(mlcache_pageget, NULL);
	tracepoint_synchronize_unregister();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Renato Costa <renatomc@cs.ubc.ca>, Jose Pazos <jpazos@cs.ubc.ca>, Surbhi Palande <csurbhi@cs.ubc.ca>");
MODULE_DESCRIPTION("This module collects data about page cache hit/misses to be used as MLCache's training data");
module_init(mlcache_init);
module_exit(mlcache_exit);
