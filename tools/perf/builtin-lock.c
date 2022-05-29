// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <inttypes.h>
#include "builtin.h"
#include "perf.h"

#include "util/evlist.h" // for struct evsel_str_handler
#include "util/evsel.h"
#include "util/symbol.h"
#include "util/thread.h"
#include "util/header.h"

#include <subcmd/pager.h>
#include <subcmd/parse-options.h>
#include "util/trace-event.h"

#include "util/debug.h"
#include "util/session.h"
#include "util/tool.h"
#include "util/data.h"
#include "util/string2.h"

#include <sys/types.h>
#include <sys/prctl.h>
#include <semaphore.h>
#include <pthread.h>
#include <math.h>
#include <limits.h>

#include <linux/list.h>
#include <linux/hash.h>
#include <linux/kernel.h>
#include <linux/zalloc.h>
#include <linux/err.h>

static struct perf_session *session;

/* based on kernel/lockdep.c */
#define LOCKHASH_BITS		12
#define LOCKHASH_SIZE		(1UL << LOCKHASH_BITS)

static struct hlist_head lockhash_table[LOCKHASH_SIZE];

#define __lockhashfn(key)	hash_long((unsigned long)key, LOCKHASH_BITS)
#define lockhashentry(key)	(lockhash_table + __lockhashfn((key)))

struct lock_stat {
	struct hlist_node	hash_entry;
	struct rb_node		rb;		/* used for sorting */

	u64			addr;		/* address of lockdep_map, used as ID */
	char			*name;		/* for strcpy(), we cannot use const */

	unsigned int		nr_acquire;
	unsigned int		nr_acquired;
	unsigned int		nr_contended;
	unsigned int		nr_release;

	unsigned int		nr_readlock;
	unsigned int		nr_trylock;

	/* these times are in nano sec. */
	u64                     avg_wait_time;
	u64			wait_time_total;
	u64			wait_time_min;
	u64			wait_time_max;

	int			broken; /* flag of blacklist */
	int			combined;
};

/*
 * States of lock_seq_stat
 *
 * UNINITIALIZED is required for detecting first event of acquire.
 * As the nature of lock events, there is no guarantee
 * that the first event for the locks are acquire,
 * it can be acquired, contended or release.
 */
#define SEQ_STATE_UNINITIALIZED      0	       /* initial state */
#define SEQ_STATE_RELEASED	1
#define SEQ_STATE_ACQUIRING	2
#define SEQ_STATE_ACQUIRED	3
#define SEQ_STATE_READ_ACQUIRED	4
#define SEQ_STATE_CONTENDED	5

/*
 * MAX_LOCK_DEPTH
 * Imported from include/linux/sched.h.
 * Should this be synchronized?
 */
#define MAX_LOCK_DEPTH 48

/*
 * struct lock_seq_stat:
 * Place to put on state of one lock sequence
 * 1) acquire -> acquired -> release
 * 2) acquire -> contended -> acquired -> release
 * 3) acquire (with read or try) -> release
 * 4) Are there other patterns?
 */
struct lock_seq_stat {
	struct list_head        list;
	int			state;
	u64			prev_event_time;
	u64                     addr;

	int                     read_count;
};

struct thread_stat {
	struct rb_node		rb;

	u32                     tid;
	struct list_head        seq_list;
};

static struct rb_root		thread_stats;

static bool combine_locks;
static bool show_thread_stats;

static struct thread_stat *thread_stat_find(u32 tid)
{
	struct rb_node *node;
	struct thread_stat *st;

	node = thread_stats.rb_node;
	while (node) {
		st = container_of(node, struct thread_stat, rb);
		if (st->tid == tid)
			return st;
		else if (tid < st->tid)
			node = node->rb_left;
		else
			node = node->rb_right;
	}

	return NULL;
}

static void thread_stat_insert(struct thread_stat *new)
{
	struct rb_node **rb = &thread_stats.rb_node;
	struct rb_node *parent = NULL;
	struct thread_stat *p;

	while (*rb) {
		p = container_of(*rb, struct thread_stat, rb);
		parent = *rb;

		if (new->tid < p->tid)
			rb = &(*rb)->rb_left;
		else if (new->tid > p->tid)
			rb = &(*rb)->rb_right;
		else
			BUG_ON("inserting invalid thread_stat\n");
	}

	rb_link_node(&new->rb, parent, rb);
	rb_insert_color(&new->rb, &thread_stats);
}

static struct thread_stat *thread_stat_findnew_after_first(u32 tid)
{
	struct thread_stat *st;

	st = thread_stat_find(tid);
	if (st)
		return st;

	st = zalloc(sizeof(struct thread_stat));
	if (!st) {
		pr_err("memory allocation failed\n");
		return NULL;
	}

	st->tid = tid;
	INIT_LIST_HEAD(&st->seq_list);

	thread_stat_insert(st);

	return st;
}

static struct thread_stat *thread_stat_findnew_first(u32 tid);
static struct thread_stat *(*thread_stat_findnew)(u32 tid) =
	thread_stat_findnew_first;

static struct thread_stat *thread_stat_findnew_first(u32 tid)
{
	struct thread_stat *st;

	st = zalloc(sizeof(struct thread_stat));
	if (!st) {
		pr_err("memory allocation failed\n");
		return NULL;
	}
	st->tid = tid;
	INIT_LIST_HEAD(&st->seq_list);

	rb_link_node(&st->rb, NULL, &thread_stats.rb_node);
	rb_insert_color(&st->rb, &thread_stats);

	thread_stat_findnew = thread_stat_findnew_after_first;
	return st;
}

/* build simple key function one is bigger than two */
#define SINGLE_KEY(member)						\
	static int lock_stat_key_ ## member(struct lock_stat *one,	\
					 struct lock_stat *two)		\
	{								\
		return one->member > two->member;			\
	}

SINGLE_KEY(nr_acquired)
SINGLE_KEY(nr_contended)
SINGLE_KEY(avg_wait_time)
SINGLE_KEY(wait_time_total)
SINGLE_KEY(wait_time_max)

static int lock_stat_key_wait_time_min(struct lock_stat *one,
					struct lock_stat *two)
{
	u64 s1 = one->wait_time_min;
	u64 s2 = two->wait_time_min;
	if (s1 == ULLONG_MAX)
		s1 = 0;
	if (s2 == ULLONG_MAX)
		s2 = 0;
	return s1 > s2;
}

struct lock_key {
	/*
	 * name: the value for specify by user
	 * this should be simpler than raw name of member
	 * e.g. nr_acquired -> acquired, wait_time_total -> wait_total
	 */
	const char		*name;
	/* header: the string printed on the header line */
	const char		*header;
	/* len: the printing width of the field */
	int			len;
	/* key: a pointer to function to compare two lock stats for sorting */
	int			(*key)(struct lock_stat*, struct lock_stat*);
	/* print: a pointer to function to print a given lock stats */
	void			(*print)(struct lock_key*, struct lock_stat*);
	/* list: list entry to link this */
	struct list_head	list;
};

#define PRINT_KEY(member)						\
static void lock_stat_key_print_ ## member(struct lock_key *key,	\
					   struct lock_stat *ls)	\
{									\
	pr_info("%*llu", key->len, (unsigned long long)ls->member);	\
}

PRINT_KEY(nr_acquired)
PRINT_KEY(nr_contended)
PRINT_KEY(avg_wait_time)
PRINT_KEY(wait_time_total)
PRINT_KEY(wait_time_max)

static void lock_stat_key_print_wait_time_min(struct lock_key *key,
					      struct lock_stat *ls)
{
	u64 wait_time = ls->wait_time_min;

	if (wait_time == ULLONG_MAX)
		wait_time = 0;

	pr_info("%*"PRIu64, key->len, wait_time);
}


static const char		*sort_key = "acquired";

static int			(*compare)(struct lock_stat *, struct lock_stat *);

static struct rb_root		sorted; /* place to store intermediate data */
static struct rb_root		result;	/* place to store sorted data */

static LIST_HEAD(lock_keys);
static const char		*output_fields;

#define DEF_KEY_LOCK(name, header, fn_suffix, len)			\
	{ #name, header, len, lock_stat_key_ ## fn_suffix, lock_stat_key_print_ ## fn_suffix, {} }
struct lock_key keys[] = {
	DEF_KEY_LOCK(acquired, "acquired", nr_acquired, 10),
	DEF_KEY_LOCK(contended, "contended", nr_contended, 10),
	DEF_KEY_LOCK(avg_wait, "avg wait (ns)", avg_wait_time, 15),
	DEF_KEY_LOCK(wait_total, "total wait (ns)", wait_time_total, 15),
	DEF_KEY_LOCK(wait_max, "max wait (ns)", wait_time_max, 15),
	DEF_KEY_LOCK(wait_min, "min wait (ns)", wait_time_min, 15),

	/* extra comparisons much complicated should be here */
	{ }
};

static int select_key(void)
{
	int i;

	for (i = 0; keys[i].name; i++) {
		if (!strcmp(keys[i].name, sort_key)) {
			compare = keys[i].key;

			/* selected key should be in the output fields */
			if (list_empty(&keys[i].list))
				list_add_tail(&keys[i].list, &lock_keys);

			return 0;
		}
	}

	pr_err("Unknown compare key: %s\n", sort_key);
	return -1;
}

static int add_output_field(struct list_head *head, char *name)
{
	int i;

	for (i = 0; keys[i].name; i++) {
		if (strcmp(keys[i].name, name))
			continue;

		/* prevent double link */
		if (list_empty(&keys[i].list))
			list_add_tail(&keys[i].list, head);

		return 0;
	}

	pr_err("Unknown output field: %s\n", name);
	return -1;
}

static int setup_output_field(const char *str)
{
	char *tok, *tmp, *orig;
	int i, ret = 0;

	/* no output field given: use all of them */
	if (str == NULL) {
		for (i = 0; keys[i].name; i++)
			list_add_tail(&keys[i].list, &lock_keys);
		return 0;
	}

	for (i = 0; keys[i].name; i++)
		INIT_LIST_HEAD(&keys[i].list);

	orig = tmp = strdup(str);
	if (orig == NULL)
		return -ENOMEM;

	while ((tok = strsep(&tmp, ",")) != NULL){
		ret = add_output_field(&lock_keys, tok);
		if (ret < 0)
			break;
	}
	free(orig);

	return ret;
}

static void combine_lock_stats(struct lock_stat *st)
{
	struct rb_node **rb = &sorted.rb_node;
	struct rb_node *parent = NULL;
	struct lock_stat *p;
	int ret;

	while (*rb) {
		p = container_of(*rb, struct lock_stat, rb);
		parent = *rb;

		if (st->name && p->name)
			ret = strcmp(st->name, p->name);
		else
			ret = !!st->name - !!p->name;

		if (ret == 0) {
			p->nr_acquired += st->nr_acquired;
			p->nr_contended += st->nr_contended;
			p->wait_time_total += st->wait_time_total;

			if (p->nr_contended)
				p->avg_wait_time = p->wait_time_total / p->nr_contended;

			if (p->wait_time_min > st->wait_time_min)
				p->wait_time_min = st->wait_time_min;
			if (p->wait_time_max < st->wait_time_max)
				p->wait_time_max = st->wait_time_max;

			p->broken |= st->broken;
			st->combined = 1;
			return;
		}

		if (ret < 0)
			rb = &(*rb)->rb_left;
		else
			rb = &(*rb)->rb_right;
	}

	rb_link_node(&st->rb, parent, rb);
	rb_insert_color(&st->rb, &sorted);
}

static void insert_to_result(struct lock_stat *st,
			     int (*bigger)(struct lock_stat *, struct lock_stat *))
{
	struct rb_node **rb = &result.rb_node;
	struct rb_node *parent = NULL;
	struct lock_stat *p;

	if (combine_locks && st->combined)
		return;

	while (*rb) {
		p = container_of(*rb, struct lock_stat, rb);
		parent = *rb;

		if (bigger(st, p))
			rb = &(*rb)->rb_left;
		else
			rb = &(*rb)->rb_right;
	}

	rb_link_node(&st->rb, parent, rb);
	rb_insert_color(&st->rb, &result);
}

/* returns left most element of result, and erase it */
static struct lock_stat *pop_from_result(void)
{
	struct rb_node *node = result.rb_node;

	if (!node)
		return NULL;

	while (node->rb_left)
		node = node->rb_left;

	rb_erase(node, &result);
	return container_of(node, struct lock_stat, rb);
}

static struct lock_stat *lock_stat_findnew(u64 addr, const char *name)
{
	struct hlist_head *entry = lockhashentry(addr);
	struct lock_stat *ret, *new;

	hlist_for_each_entry(ret, entry, hash_entry) {
		if (ret->addr == addr)
			return ret;
	}

	new = zalloc(sizeof(struct lock_stat));
	if (!new)
		goto alloc_failed;

	new->addr = addr;
	new->name = zalloc(sizeof(char) * strlen(name) + 1);
	if (!new->name) {
		free(new);
		goto alloc_failed;
	}

	strcpy(new->name, name);
	new->wait_time_min = ULLONG_MAX;

	hlist_add_head(&new->hash_entry, entry);
	return new;

alloc_failed:
	pr_err("memory allocation failed\n");
	return NULL;
}

struct trace_lock_handler {
	int (*acquire_event)(struct evsel *evsel,
			     struct perf_sample *sample);

	int (*acquired_event)(struct evsel *evsel,
			      struct perf_sample *sample);

	int (*contended_event)(struct evsel *evsel,
			       struct perf_sample *sample);

	int (*release_event)(struct evsel *evsel,
			     struct perf_sample *sample);
};

static struct lock_seq_stat *get_seq(struct thread_stat *ts, u64 addr)
{
	struct lock_seq_stat *seq;

	list_for_each_entry(seq, &ts->seq_list, list) {
		if (seq->addr == addr)
			return seq;
	}

	seq = zalloc(sizeof(struct lock_seq_stat));
	if (!seq) {
		pr_err("memory allocation failed\n");
		return NULL;
	}
	seq->state = SEQ_STATE_UNINITIALIZED;
	seq->addr = addr;

	list_add(&seq->list, &ts->seq_list);
	return seq;
}

enum broken_state {
	BROKEN_ACQUIRE,
	BROKEN_ACQUIRED,
	BROKEN_CONTENDED,
	BROKEN_RELEASE,
	BROKEN_MAX,
};

static int bad_hist[BROKEN_MAX];

enum acquire_flags {
	TRY_LOCK = 1,
	READ_LOCK = 2,
};

static int report_lock_acquire_event(struct evsel *evsel,
				     struct perf_sample *sample)
{
	struct lock_stat *ls;
	struct thread_stat *ts;
	struct lock_seq_stat *seq;
	const char *name = evsel__strval(evsel, sample, "name");
	u64 addr = evsel__intval(evsel, sample, "lockdep_addr");
	int flag = evsel__intval(evsel, sample, "flags");

	/* abuse ls->addr for tid */
	if (show_thread_stats)
		addr = sample->tid;

	ls = lock_stat_findnew(addr, name);
	if (!ls)
		return -ENOMEM;

	ts = thread_stat_findnew(sample->tid);
	if (!ts)
		return -ENOMEM;

	seq = get_seq(ts, addr);
	if (!seq)
		return -ENOMEM;

	switch (seq->state) {
	case SEQ_STATE_UNINITIALIZED:
	case SEQ_STATE_RELEASED:
		if (!flag) {
			seq->state = SEQ_STATE_ACQUIRING;
		} else {
			if (flag & TRY_LOCK)
				ls->nr_trylock++;
			if (flag & READ_LOCK)
				ls->nr_readlock++;
			seq->state = SEQ_STATE_READ_ACQUIRED;
			seq->read_count = 1;
			ls->nr_acquired++;
		}
		break;
	case SEQ_STATE_READ_ACQUIRED:
		if (flag & READ_LOCK) {
			seq->read_count++;
			ls->nr_acquired++;
			goto end;
		} else {
			goto broken;
		}
		break;
	case SEQ_STATE_ACQUIRED:
	case SEQ_STATE_ACQUIRING:
	case SEQ_STATE_CONTENDED:
broken:
		/* broken lock sequence */
		if (!ls->broken) {
			ls->broken = 1;
			bad_hist[BROKEN_ACQUIRE]++;
		}
		list_del_init(&seq->list);
		free(seq);
		goto end;
	default:
		BUG_ON("Unknown state of lock sequence found!\n");
		break;
	}

	ls->nr_acquire++;
	seq->prev_event_time = sample->time;
end:
	return 0;
}

static int report_lock_acquired_event(struct evsel *evsel,
				      struct perf_sample *sample)
{
	struct lock_stat *ls;
	struct thread_stat *ts;
	struct lock_seq_stat *seq;
	u64 contended_term;
	const char *name = evsel__strval(evsel, sample, "name");
	u64 addr = evsel__intval(evsel, sample, "lockdep_addr");

	if (show_thread_stats)
		addr = sample->tid;

	ls = lock_stat_findnew(addr, name);
	if (!ls)
		return -ENOMEM;

	ts = thread_stat_findnew(sample->tid);
	if (!ts)
		return -ENOMEM;

	seq = get_seq(ts, addr);
	if (!seq)
		return -ENOMEM;

	switch (seq->state) {
	case SEQ_STATE_UNINITIALIZED:
		/* orphan event, do nothing */
		return 0;
	case SEQ_STATE_ACQUIRING:
		break;
	case SEQ_STATE_CONTENDED:
		contended_term = sample->time - seq->prev_event_time;
		ls->wait_time_total += contended_term;
		if (contended_term < ls->wait_time_min)
			ls->wait_time_min = contended_term;
		if (ls->wait_time_max < contended_term)
			ls->wait_time_max = contended_term;
		break;
	case SEQ_STATE_RELEASED:
	case SEQ_STATE_ACQUIRED:
	case SEQ_STATE_READ_ACQUIRED:
		/* broken lock sequence */
		if (!ls->broken) {
			ls->broken = 1;
			bad_hist[BROKEN_ACQUIRED]++;
		}
		list_del_init(&seq->list);
		free(seq);
		goto end;
	default:
		BUG_ON("Unknown state of lock sequence found!\n");
		break;
	}

	seq->state = SEQ_STATE_ACQUIRED;
	ls->nr_acquired++;
	ls->avg_wait_time = ls->nr_contended ? ls->wait_time_total/ls->nr_contended : 0;
	seq->prev_event_time = sample->time;
end:
	return 0;
}

static int report_lock_contended_event(struct evsel *evsel,
				       struct perf_sample *sample)
{
	struct lock_stat *ls;
	struct thread_stat *ts;
	struct lock_seq_stat *seq;
	const char *name = evsel__strval(evsel, sample, "name");
	u64 addr = evsel__intval(evsel, sample, "lockdep_addr");

	if (show_thread_stats)
		addr = sample->tid;

	ls = lock_stat_findnew(addr, name);
	if (!ls)
		return -ENOMEM;

	ts = thread_stat_findnew(sample->tid);
	if (!ts)
		return -ENOMEM;

	seq = get_seq(ts, addr);
	if (!seq)
		return -ENOMEM;

	switch (seq->state) {
	case SEQ_STATE_UNINITIALIZED:
		/* orphan event, do nothing */
		return 0;
	case SEQ_STATE_ACQUIRING:
		break;
	case SEQ_STATE_RELEASED:
	case SEQ_STATE_ACQUIRED:
	case SEQ_STATE_READ_ACQUIRED:
	case SEQ_STATE_CONTENDED:
		/* broken lock sequence */
		if (!ls->broken) {
			ls->broken = 1;
			bad_hist[BROKEN_CONTENDED]++;
		}
		list_del_init(&seq->list);
		free(seq);
		goto end;
	default:
		BUG_ON("Unknown state of lock sequence found!\n");
		break;
	}

	seq->state = SEQ_STATE_CONTENDED;
	ls->nr_contended++;
	ls->avg_wait_time = ls->wait_time_total/ls->nr_contended;
	seq->prev_event_time = sample->time;
end:
	return 0;
}

static int report_lock_release_event(struct evsel *evsel,
				     struct perf_sample *sample)
{
	struct lock_stat *ls;
	struct thread_stat *ts;
	struct lock_seq_stat *seq;
	const char *name = evsel__strval(evsel, sample, "name");
	u64 addr = evsel__intval(evsel, sample, "lockdep_addr");

	if (show_thread_stats)
		addr = sample->tid;

	ls = lock_stat_findnew(addr, name);
	if (!ls)
		return -ENOMEM;

	ts = thread_stat_findnew(sample->tid);
	if (!ts)
		return -ENOMEM;

	seq = get_seq(ts, addr);
	if (!seq)
		return -ENOMEM;

	switch (seq->state) {
	case SEQ_STATE_UNINITIALIZED:
		goto end;
	case SEQ_STATE_ACQUIRED:
		break;
	case SEQ_STATE_READ_ACQUIRED:
		seq->read_count--;
		BUG_ON(seq->read_count < 0);
		if (seq->read_count) {
			ls->nr_release++;
			goto end;
		}
		break;
	case SEQ_STATE_ACQUIRING:
	case SEQ_STATE_CONTENDED:
	case SEQ_STATE_RELEASED:
		/* broken lock sequence */
		if (!ls->broken) {
			ls->broken = 1;
			bad_hist[BROKEN_RELEASE]++;
		}
		goto free_seq;
	default:
		BUG_ON("Unknown state of lock sequence found!\n");
		break;
	}

	ls->nr_release++;
free_seq:
	list_del_init(&seq->list);
	free(seq);
end:
	return 0;
}

/* lock oriented handlers */
/* TODO: handlers for CPU oriented, thread oriented */
static struct trace_lock_handler report_lock_ops  = {
	.acquire_event		= report_lock_acquire_event,
	.acquired_event		= report_lock_acquired_event,
	.contended_event	= report_lock_contended_event,
	.release_event		= report_lock_release_event,
};

static struct trace_lock_handler *trace_handler;

static int evsel__process_lock_acquire(struct evsel *evsel, struct perf_sample *sample)
{
	if (trace_handler->acquire_event)
		return trace_handler->acquire_event(evsel, sample);
	return 0;
}

static int evsel__process_lock_acquired(struct evsel *evsel, struct perf_sample *sample)
{
	if (trace_handler->acquired_event)
		return trace_handler->acquired_event(evsel, sample);
	return 0;
}

static int evsel__process_lock_contended(struct evsel *evsel, struct perf_sample *sample)
{
	if (trace_handler->contended_event)
		return trace_handler->contended_event(evsel, sample);
	return 0;
}

static int evsel__process_lock_release(struct evsel *evsel, struct perf_sample *sample)
{
	if (trace_handler->release_event)
		return trace_handler->release_event(evsel, sample);
	return 0;
}

static void print_bad_events(int bad, int total)
{
	/* Output for debug, this have to be removed */
	int i;
	const char *name[4] =
		{ "acquire", "acquired", "contended", "release" };

	pr_info("\n=== output for debug===\n\n");
	pr_info("bad: %d, total: %d\n", bad, total);
	pr_info("bad rate: %.2f %%\n", (double)bad / (double)total * 100);
	pr_info("histogram of events caused bad sequence\n");
	for (i = 0; i < BROKEN_MAX; i++)
		pr_info(" %10s: %d\n", name[i], bad_hist[i]);
}

/* TODO: various way to print, coloring, nano or milli sec */
static void print_result(void)
{
	struct lock_stat *st;
	struct lock_key *key;
	char cut_name[20];
	int bad, total;

	pr_info("%20s ", "Name");
	list_for_each_entry(key, &lock_keys, list)
		pr_info("%*s ", key->len, key->header);
	pr_info("\n\n");

	bad = total = 0;
	while ((st = pop_from_result())) {
		total++;
		if (st->broken)
			bad++;
		if (!st->nr_acquired)
			continue;

		bzero(cut_name, 20);

		if (strlen(st->name) < 20) {
			/* output raw name */
			const char *name = st->name;

			if (show_thread_stats) {
				struct thread *t;

				/* st->addr contains tid of thread */
				t = perf_session__findnew(session, st->addr);
				name = thread__comm_str(t);
			}

			pr_info("%20s ", name);
		} else {
			strncpy(cut_name, st->name, 16);
			cut_name[16] = '.';
			cut_name[17] = '.';
			cut_name[18] = '.';
			cut_name[19] = '\0';
			/* cut off name for saving output style */
			pr_info("%20s ", cut_name);
		}

		list_for_each_entry(key, &lock_keys, list) {
			key->print(key, st);
			pr_info(" ");
		}
		pr_info("\n");
	}

	print_bad_events(bad, total);
}

static bool info_threads, info_map;

static void dump_threads(void)
{
	struct thread_stat *st;
	struct rb_node *node;
	struct thread *t;

	pr_info("%10s: comm\n", "Thread ID");

	node = rb_first(&thread_stats);
	while (node) {
		st = container_of(node, struct thread_stat, rb);
		t = perf_session__findnew(session, st->tid);
		pr_info("%10d: %s\n", st->tid, thread__comm_str(t));
		node = rb_next(node);
		thread__put(t);
	}
}

static int compare_maps(struct lock_stat *a, struct lock_stat *b)
{
	int ret;

	if (a->name && b->name)
		ret = strcmp(a->name, b->name);
	else
		ret = !!a->name - !!b->name;

	if (!ret)
		return a->addr < b->addr;
	else
		return ret < 0;
}

static void dump_map(void)
{
	unsigned int i;
	struct lock_stat *st;

	pr_info("Address of instance: name of class\n");
	for (i = 0; i < LOCKHASH_SIZE; i++) {
		hlist_for_each_entry(st, &lockhash_table[i], hash_entry) {
			insert_to_result(st, compare_maps);
		}
	}

	while ((st = pop_from_result()))
		pr_info(" %#llx: %s\n", (unsigned long long)st->addr, st->name);
}

static int dump_info(void)
{
	int rc = 0;

	if (info_threads)
		dump_threads();
	else if (info_map)
		dump_map();
	else {
		rc = -1;
		pr_err("Unknown type of information\n");
	}

	return rc;
}

typedef int (*tracepoint_handler)(struct evsel *evsel,
				  struct perf_sample *sample);

static int process_sample_event(struct perf_tool *tool __maybe_unused,
				union perf_event *event,
				struct perf_sample *sample,
				struct evsel *evsel,
				struct machine *machine)
{
	int err = 0;
	struct thread *thread = machine__findnew_thread(machine, sample->pid,
							sample->tid);

	if (thread == NULL) {
		pr_debug("problem processing %d event, skipping it.\n",
			event->header.type);
		return -1;
	}

	if (evsel->handler != NULL) {
		tracepoint_handler f = evsel->handler;
		err = f(evsel, sample);
	}

	thread__put(thread);

	return err;
}

static void combine_result(void)
{
	unsigned int i;
	struct lock_stat *st;

	if (!combine_locks)
		return;

	for (i = 0; i < LOCKHASH_SIZE; i++) {
		hlist_for_each_entry(st, &lockhash_table[i], hash_entry) {
			combine_lock_stats(st);
		}
	}
}

static void sort_result(void)
{
	unsigned int i;
	struct lock_stat *st;

	for (i = 0; i < LOCKHASH_SIZE; i++) {
		hlist_for_each_entry(st, &lockhash_table[i], hash_entry) {
			insert_to_result(st, compare);
		}
	}
}

static const struct evsel_str_handler lock_tracepoints[] = {
	{ "lock:lock_acquire",	 evsel__process_lock_acquire,   }, /* CONFIG_LOCKDEP */
	{ "lock:lock_acquired",	 evsel__process_lock_acquired,  }, /* CONFIG_LOCKDEP, CONFIG_LOCK_STAT */
	{ "lock:lock_contended", evsel__process_lock_contended, }, /* CONFIG_LOCKDEP, CONFIG_LOCK_STAT */
	{ "lock:lock_release",	 evsel__process_lock_release,   }, /* CONFIG_LOCKDEP */
};

static bool force;

static int __cmd_report(bool display_info)
{
	int err = -EINVAL;
	struct perf_tool eops = {
		.sample		 = process_sample_event,
		.comm		 = perf_event__process_comm,
		.namespaces	 = perf_event__process_namespaces,
		.ordered_events	 = true,
	};
	struct perf_data data = {
		.path  = input_name,
		.mode  = PERF_DATA_MODE_READ,
		.force = force,
	};

	session = perf_session__new(&data, &eops);
	if (IS_ERR(session)) {
		pr_err("Initializing perf session failed\n");
		return PTR_ERR(session);
	}

	symbol__init(&session->header.env);

	if (!perf_session__has_traces(session, "lock record"))
		goto out_delete;

	if (perf_session__set_tracepoints_handlers(session, lock_tracepoints)) {
		pr_err("Initializing perf session tracepoint handlers failed\n");
		goto out_delete;
	}

	if (setup_output_field(output_fields))
		goto out_delete;

	if (select_key())
		goto out_delete;

	err = perf_session__process_events(session);
	if (err)
		goto out_delete;

	setup_pager();
	if (display_info) /* used for info subcommand */
		err = dump_info();
	else {
		combine_result();
		sort_result();
		print_result();
	}

out_delete:
	perf_session__delete(session);
	return err;
}

static int __cmd_record(int argc, const char **argv)
{
	const char *record_args[] = {
		"record", "-R", "-m", "1024", "-c", "1", "--synth", "no",
	};
	unsigned int rec_argc, i, j, ret;
	const char **rec_argv;

	for (i = 0; i < ARRAY_SIZE(lock_tracepoints); i++) {
		if (!is_valid_tracepoint(lock_tracepoints[i].name)) {
				pr_err("tracepoint %s is not enabled. "
				       "Are CONFIG_LOCKDEP and CONFIG_LOCK_STAT enabled?\n",
				       lock_tracepoints[i].name);
				return 1;
		}
	}

	rec_argc = ARRAY_SIZE(record_args) + argc - 1;
	/* factor of 2 is for -e in front of each tracepoint */
	rec_argc += 2 * ARRAY_SIZE(lock_tracepoints);

	rec_argv = calloc(rec_argc + 1, sizeof(char *));
	if (!rec_argv)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(record_args); i++)
		rec_argv[i] = strdup(record_args[i]);

	for (j = 0; j < ARRAY_SIZE(lock_tracepoints); j++) {
		rec_argv[i++] = "-e";
		rec_argv[i++] = strdup(lock_tracepoints[j].name);
	}

	for (j = 1; j < (unsigned int)argc; j++, i++)
		rec_argv[i] = argv[j];

	BUG_ON(i != rec_argc);

	ret = cmd_record(i, rec_argv);
	free(rec_argv);
	return ret;
}

int cmd_lock(int argc, const char **argv)
{
	const struct option lock_options[] = {
	OPT_STRING('i', "input", &input_name, "file", "input file name"),
	OPT_INCR('v', "verbose", &verbose, "be more verbose (show symbol address, etc)"),
	OPT_BOOLEAN('D', "dump-raw-trace", &dump_trace, "dump raw trace in ASCII"),
	OPT_BOOLEAN('f', "force", &force, "don't complain, do it"),
	OPT_END()
	};

	const struct option info_options[] = {
	OPT_BOOLEAN('t', "threads", &info_threads,
		    "dump thread list in perf.data"),
	OPT_BOOLEAN('m', "map", &info_map,
		    "map of lock instances (address:name table)"),
	OPT_PARENT(lock_options)
	};

	const struct option report_options[] = {
	OPT_STRING('k', "key", &sort_key, "acquired",
		    "key for sorting (acquired / contended / avg_wait / wait_total / wait_max / wait_min)"),
	OPT_STRING('F', "field", &output_fields, NULL,
		    "output fields (acquired / contended / avg_wait / wait_total / wait_max / wait_min)"),
	/* TODO: type */
	OPT_BOOLEAN('c', "combine-locks", &combine_locks,
		    "combine locks in the same class"),
	OPT_BOOLEAN('t', "threads", &show_thread_stats,
		    "show per-thread lock stats"),
	OPT_PARENT(lock_options)
	};

	const char * const info_usage[] = {
		"perf lock info [<options>]",
		NULL
	};
	const char *const lock_subcommands[] = { "record", "report", "script",
						 "info", NULL };
	const char *lock_usage[] = {
		NULL,
		NULL
	};
	const char * const report_usage[] = {
		"perf lock report [<options>]",
		NULL
	};
	unsigned int i;
	int rc = 0;

	for (i = 0; i < LOCKHASH_SIZE; i++)
		INIT_HLIST_HEAD(lockhash_table + i);

	argc = parse_options_subcommand(argc, argv, lock_options, lock_subcommands,
					lock_usage, PARSE_OPT_STOP_AT_NON_OPTION);
	if (!argc)
		usage_with_options(lock_usage, lock_options);

	if (strlen(argv[0]) > 2 && strstarts("record", argv[0])) {
		return __cmd_record(argc, argv);
	} else if (strlen(argv[0]) > 2 && strstarts("report", argv[0])) {
		trace_handler = &report_lock_ops;
		if (argc) {
			argc = parse_options(argc, argv,
					     report_options, report_usage, 0);
			if (argc)
				usage_with_options(report_usage, report_options);
		}
		rc = __cmd_report(false);
	} else if (!strcmp(argv[0], "script")) {
		/* Aliased to 'perf script' */
		return cmd_script(argc, argv);
	} else if (!strcmp(argv[0], "info")) {
		if (argc) {
			argc = parse_options(argc, argv,
					     info_options, info_usage, 0);
			if (argc)
				usage_with_options(info_usage, info_options);
		}
		/* recycling report_lock_ops */
		trace_handler = &report_lock_ops;
		rc = __cmd_report(true);
	} else {
		usage_with_options(lock_usage, lock_options);
	}

	return rc;
}
