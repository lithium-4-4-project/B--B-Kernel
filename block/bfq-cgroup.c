/*
 * BFQ: CGROUPS support.
 *
 * Based on ideas and code from CFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2015 Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2016 Paolo Valente <paolo.valente@linaro.org>
 *
 * Licensed under the GPL-2 as detailed in the accompanying COPYING.BFQ
 * file.
 */

#ifdef CONFIG_BFQ_GROUP_IOSCHED

/* bfqg stats flags */
enum bfqg_stats_flags {
	BFQG_stats_waiting = 0,
	BFQG_stats_idling,
	BFQG_stats_empty,
};

#define BFQG_FLAG_FNS(name)						\
static void bfqg_stats_mark_##name(struct bfqg_stats *stats)	\
{									\
	stats->flags |= (1 << BFQG_stats_##name);			\
}									\
static void bfqg_stats_clear_##name(struct bfqg_stats *stats)	\
{									\
	stats->flags &= ~(1 << BFQG_stats_##name);			\
}									\
static int bfqg_stats_##name(struct bfqg_stats *stats)		\
{									\
	return (stats->flags & (1 << BFQG_stats_##name)) != 0;		\
}									\

BFQG_FLAG_FNS(waiting)
BFQG_FLAG_FNS(idling)
BFQG_FLAG_FNS(empty)
#undef BFQG_FLAG_FNS

/* This should be called with the queue_lock held. */
static void bfqg_stats_update_group_wait_time(struct bfqg_stats *stats)
{
	unsigned long long now;

	if (!bfqg_stats_waiting(stats))
		return;

	now = sched_clock();
	if (time_after64(now, stats->start_group_wait_time))
		blkg_stat_add(&stats->group_wait_time,
			      now - stats->start_group_wait_time);
	bfqg_stats_clear_waiting(stats);
}

/* This should be called with the queue_lock held. */
static void bfqg_stats_set_start_group_wait_time(struct bfq_group *bfqg,
						 struct bfq_group *curr_bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	if (bfqg_stats_waiting(stats))
		return;
	if (bfqg == curr_bfqg)
		return;
	stats->start_group_wait_time = sched_clock();
	bfqg_stats_mark_waiting(stats);
}

/* This should be called with the queue_lock held. */
static void bfqg_stats_end_empty_time(struct bfqg_stats *stats)
{
	unsigned long long now;

	if (!bfqg_stats_empty(stats))
		return;

	now = sched_clock();
	if (time_after64(now, stats->start_empty_time))
		blkg_stat_add(&stats->empty_time,
			      now - stats->start_empty_time);
	bfqg_stats_clear_empty(stats);
}

static void bfqg_stats_update_dequeue(struct bfq_group *bfqg)
{
	blkg_stat_add(&bfqg->stats.dequeue, 1);
}

static void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	if (blkg_rwstat_total(&stats->queued))
		return;

	/*
	 * group is already marked empty. This can happen if bfqq got new
	 * request in parent group and moved to this group while being added
	 * to service tree. Just ignore the event and move on.
	 */
	if (bfqg_stats_empty(stats))
		return;

	stats->start_empty_time = sched_clock();
	bfqg_stats_mark_empty(stats);
}

static void bfqg_stats_update_idle_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	if (bfqg_stats_idling(stats)) {
		unsigned long long now = sched_clock();

		if (time_after64(now, stats->start_idle_time))
			blkg_stat_add(&stats->idle_time,
				      now - stats->start_idle_time);
		bfqg_stats_clear_idling(stats);
	}
}

static void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	stats->start_idle_time = sched_clock();
	bfqg_stats_mark_idling(stats);
}

static void bfqg_stats_update_avg_queue_size(struct bfq_group *bfqg)
{
	struct bfqg_stats *stats = &bfqg->stats;

	blkg_stat_add(&stats->avg_queue_size_sum,
		      blkg_rwstat_total(&stats->queued));
	blkg_stat_add(&stats->avg_queue_size_samples, 1);
	bfqg_stats_update_group_wait_time(stats);
}

static struct blkcg_policy blkcg_policy_bfq;

/*
 * blk-cgroup policy-related handlers
 * The following functions help in converting between blk-cgroup
 * internal structures and BFQ-specific structures.
 */

static struct bfq_group *pd_to_bfqg(struct blkg_policy_data *pd)
{
	return pd ? container_of(pd, struct bfq_group, pd) : NULL;
}

static struct blkcg_gq *bfqg_to_blkg(struct bfq_group *bfqg)
{
	return pd_to_blkg(&bfqg->pd);
}

static struct bfq_group *blkg_to_bfqg(struct blkcg_gq *blkg)
{
	struct blkg_policy_data *pd = blkg_to_pd(blkg, &blkcg_policy_bfq);

	return pd_to_bfqg(pd);
}

/*
 * bfq_group handlers
 * The following functions help in navigating the bfq_group hierarchy
 * by allowing to find the parent of a bfq_group or the bfq_group
 * associated to a bfq_queue.
 */

static struct bfq_group *bfqg_parent(struct bfq_group *bfqg)
{
	struct blkcg_gq *pblkg = bfqg_to_blkg(bfqg)->parent;

	return pblkg ? blkg_to_bfqg(pblkg) : NULL;
}

static struct bfq_group *bfqq_group(struct bfq_queue *bfqq)
{
	struct bfq_entity *group_entity = bfqq->entity.parent;

	return group_entity ? container_of(group_entity, struct bfq_group,
					   entity) :
			      bfqq->bfqd->root_group;
}

/*
 * The following two functions handle get and put of a bfq_group by
 * wrapping the related blk-cgroup hooks.
 */

static void bfqg_get(struct bfq_group *bfqg)
{
	return blkg_get(bfqg_to_blkg(bfqg));
}

static void bfqg_put(struct bfq_group *bfqg)
{
	return blkg_put(bfqg_to_blkg(bfqg));
}

static void bfqg_stats_update_io_add(struct bfq_group *bfqg,
				     struct bfq_queue *bfqq,
				     int op, int op_flags)
{
	blkg_rwstat_add(&bfqg->stats.queued, op, op_flags, 1);
	bfqg_stats_end_empty_time(&bfqg->stats);
	if (!(bfqq == ((struct bfq_data *)bfqg->bfqd)->in_service_queue))
		bfqg_stats_set_start_group_wait_time(bfqg, bfqq_group(bfqq));
}

static void bfqg_stats_update_io_remove(struct bfq_group *bfqg, int op,
										int op_flags)
{
	blkg_rwstat_add(&bfqg->stats.queued, op, op_flags, -1);
}

static void bfqg_stats_update_io_merged(struct bfq_group *bfqg, int op,
										int op_flags)
{
	blkg_rwstat_add(&bfqg->stats.merged, op, op_flags, 1);
}

static void bfqg_stats_update_completion(struct bfq_group *bfqg,
			uint64_t start_time, uint64_t io_start_time, int op,
			int op_flags)
{
	struct bfqg_stats *stats = &bfqg->stats;
	unsigned long long now = sched_clock();

	if (time_after64(now, io_start_time))
		blkg_rwstat_add(&stats->service_time, op, op_flags,
						now - io_start_time);
	if (time_after64(io_start_time, start_time))
		blkg_rwstat_add(&stats->wait_time, op, op_flags,
				io_start_time - start_time);
}

/* @stats = 0 */
static void bfqg_stats_reset(struct bfqg_stats *stats)
{
	/* queued stats shouldn't be cleared */
	blkg_rwstat_reset(&stats->merged);
	blkg_rwstat_reset(&stats->service_time);
	blkg_rwstat_reset(&stats->wait_time);
	blkg_stat_reset(&stats->time);
	blkg_stat_reset(&stats->avg_queue_size_sum);
	blkg_stat_reset(&stats->avg_queue_size_samples);
	blkg_stat_reset(&stats->dequeue);
	blkg_stat_reset(&stats->group_wait_time);
	blkg_stat_reset(&stats->idle_time);
	blkg_stat_reset(&stats->empty_time);
}

/* @to += @from */
static void bfqg_stats_add_aux(struct bfqg_stats *to, struct bfqg_stats *from)
{
	if (!to || !from)
		return;

	/* queued stats shouldn't be cleared */
	blkg_rwstat_add_aux(&to->merged, &from->merged);
	blkg_rwstat_add_aux(&to->service_time, &from->service_time);
	blkg_rwstat_add_aux(&to->wait_time, &from->wait_time);
	blkg_stat_add_aux(&from->time, &from->time);
	blkg_stat_add_aux(&to->avg_queue_size_sum, &from->avg_queue_size_sum);
	blkg_stat_add_aux(&to->avg_queue_size_samples, &from->avg_queue_size_samples);
	blkg_stat_add_aux(&to->dequeue, &from->dequeue);
	blkg_stat_add_aux(&to->group_wait_time, &from->group_wait_time);
	blkg_stat_add_aux(&to->idle_time, &from->idle_time);
	blkg_stat_add_aux(&to->empty_time, &from->empty_time);
}

/*
 * Transfer @bfqg's stats to its parent's dead_stats so that the ancestors'
 * recursive stats can still account for the amount used by this bfqg after
 * it's gone.
 */
static void bfqg_stats_xfer_dead(struct bfq_group *bfqg)
{
	struct bfq_group *parent;

	if (!bfqg) /* root_group */
		return;

	parent = bfqg_parent(bfqg);

	lockdep_assert_held(bfqg_to_blkg(bfqg)->q->queue_lock);

	if (unlikely(!parent))
		return;

	bfqg_stats_add_aux(&parent->stats, &bfqg->stats);
	bfqg_stats_reset(&bfqg->stats);
}

static void bfq_init_entity(struct bfq_entity *entity,
			    struct bfq_group *bfqg)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	entity->weight = entity->new_weight;
	entity->orig_weight = entity->new_weight;
	if (bfqq) {
		bfqq->ioprio = bfqq->new_ioprio;
		bfqq->ioprio_class = bfqq->new_ioprio_class;
		bfqg_get(bfqg);
	}
	entity->parent = bfqg->my_entity; /* NULL for root group */
	entity->sched_data = &bfqg->sched_data;
}

static void bfqg_stats_exit(struct bfqg_stats *stats)
{
	blkg_rwstat_exit(&stats->merged);
	blkg_rwstat_exit(&stats->service_time);
	blkg_rwstat_exit(&stats->wait_time);
	blkg_rwstat_exit(&stats->queued);
	blkg_stat_exit(&stats->time);
	blkg_stat_exit(&stats->avg_queue_size_sum);
	blkg_stat_exit(&stats->avg_queue_size_samples);
	blkg_stat_exit(&stats->dequeue);
	blkg_stat_exit(&stats->group_wait_time);
	blkg_stat_exit(&stats->idle_time);
	blkg_stat_exit(&stats->empty_time);
}

static int bfqg_stats_init(struct bfqg_stats *stats, gfp_t gfp)
{
	if (blkg_rwstat_init(&stats->merged, gfp) ||
	    blkg_rwstat_init(&stats->service_time, gfp) ||
	    blkg_rwstat_init(&stats->wait_time, gfp) ||
	    blkg_rwstat_init(&stats->queued, gfp) ||
	    blkg_stat_init(&stats->time, gfp) ||
	    blkg_stat_init(&stats->avg_queue_size_sum, gfp) ||
	    blkg_stat_init(&stats->avg_queue_size_samples, gfp) ||
	    blkg_stat_init(&stats->dequeue, gfp) ||
	    blkg_stat_init(&stats->group_wait_time, gfp) ||
	    blkg_stat_init(&stats->idle_time, gfp) ||
	    blkg_stat_init(&stats->empty_time, gfp)) {
		bfqg_stats_exit(stats);
		return -ENOMEM;
	}

	return 0;
}

static struct bfq_group_data *cpd_to_bfqgd(struct blkcg_policy_data *cpd)
 {
	return cpd ? container_of(cpd, struct bfq_group_data, pd) : NULL;
 }

static struct bfq_group_data *blkcg_to_bfqgd(struct blkcg *blkcg)
{
	return cpd_to_bfqgd(blkcg_to_cpd(blkcg, &blkcg_policy_bfq));
}

static struct blkcg_policy_data *bfq_cpd_alloc(gfp_t gfp)
{
	struct bfq_group_data *bgd;

	bgd = kzalloc(sizeof(*bgd), GFP_KERNEL);
	if (!bgd)
		return NULL;
	return &bgd->pd;
}

static void bfq_cpd_init(struct blkcg_policy_data *cpd)
{
	struct bfq_group_data *d = cpd_to_bfqgd(cpd);

	d->weight = cgroup_subsys_on_dfl(io_cgrp_subsys) ?
		CGROUP_WEIGHT_DFL : BFQ_WEIGHT_LEGACY_DFL;
}

static void bfq_cpd_free(struct blkcg_policy_data *cpd)
{
	kfree(cpd_to_bfqgd(cpd));
}

static struct blkg_policy_data *bfq_pd_alloc(gfp_t gfp, int node)
{
	struct bfq_group *bfqg;

	bfqg = kzalloc_node(sizeof(*bfqg), gfp, node);
	if (!bfqg)
		return NULL;

	if (bfqg_stats_init(&bfqg->stats, gfp)) {
		kfree(bfqg);
		return NULL;
	}

	return &bfqg->pd;
}

static void bfq_pd_init(struct blkg_policy_data *pd)
{
	struct blkcg_gq *blkg;
	struct bfq_group *bfqg;
	struct bfq_data *bfqd;
	struct bfq_entity *entity;
	struct bfq_group_data *d;

	blkg = pd_to_blkg(pd);
	BUG_ON(!blkg);
	bfqg = blkg_to_bfqg(blkg);
	bfqd = blkg->q->elevator->elevator_data;
	entity = &bfqg->entity;
	d = blkcg_to_bfqgd(blkg->blkcg);

	entity->orig_weight = entity->weight = entity->new_weight = d->weight;
	entity->my_sched_data = &bfqg->sched_data;
	bfqg->my_entity = entity; /*
				   * the root_group's will be set to NULL
				   * in bfq_init_queue()
				   */
	bfqg->bfqd = bfqd;
	bfqg->active_entities = 0;
	bfqg->rq_pos_tree = RB_ROOT;
}

static void bfq_pd_free(struct blkg_policy_data *pd)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);

	bfqg_stats_exit(&bfqg->stats);

	return kfree(bfqg);
}

static void bfq_pd_reset_stats(struct blkg_policy_data *pd)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);

	bfqg_stats_reset(&bfqg->stats);
}

static void bfq_group_set_parent(struct bfq_group *bfqg,
				 struct bfq_group *parent)
{
	struct bfq_entity *entity;
	BUG_ON(!parent);
	BUG_ON(!bfqg);
	BUG_ON(bfqg == parent);

	entity = &bfqg->entity;
	entity->parent = parent->my_entity;
	entity->sched_data = &parent->sched_data;
}

static struct bfq_group *bfq_lookup_bfqg(struct bfq_data *bfqd,
					 struct blkcg *blkcg)
{
	struct blkcg_gq *blkg;

	blkg = blkg_lookup(blkcg, bfqd->queue);
	if (likely(blkg))
		return blkg_to_bfqg(blkg);
	return NULL;
}

static struct bfq_group *bfq_find_set_group(struct bfq_data *bfqd,
					    struct blkcg *blkcg)
{
	struct bfq_group *bfqg, *parent;
	struct bfq_entity *entity;

	assert_spin_locked(bfqd->queue->queue_lock);

	bfqg = bfq_lookup_bfqg(bfqd, blkcg);

	if (unlikely(!bfqg))
		return NULL;

	/*
	 * Update chain of bfq_groups as we might be handling a leaf group
	 * which, along with some of its relatives, has not been hooked yet
	 * to the private hierarchy of BFQ.
	 */
	entity = &bfqg->entity;
	for_each_entity(entity) {
		bfqg = container_of(entity, struct bfq_group, entity);
		BUG_ON(!bfqg);
		if (bfqg != bfqd->root_group) {
			parent = bfqg_parent(bfqg);
			if (!parent)
				parent = bfqd->root_group;
			BUG_ON(!parent);
			bfq_group_set_parent(bfqg, parent);
		}
	}

	return bfqg;
}

static void bfq_pos_tree_add_move(struct bfq_data *bfqd,
				  struct bfq_queue *bfqq);

static void bfq_bfqq_expire(struct bfq_data *bfqd,
			    struct bfq_queue *bfqq,
			    bool compensate,
			    enum bfqq_expiration reason);

/**
 * bfq_bfqq_move - migrate @bfqq to @bfqg.
 * @bfqd: queue descriptor.
 * @bfqq: the queue to move.
 * @bfqg: the group to move to.
 *
 * Move @bfqq to @bfqg, deactivating it from its old group and reactivating
 * it on the new one.  Avoid putting the entity on the old group idle tree.
 *
 * Must be called under the queue lock; the cgroup owning @bfqg must
 * not disappear (by now this just means that we are called under
 * rcu_read_lock()).
 */
static void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  struct bfq_group *bfqg)
{
	struct bfq_entity *entity = &bfqq->entity;

	BUG_ON(!bfq_bfqq_busy(bfqq) && !RB_EMPTY_ROOT(&bfqq->sort_list));
	BUG_ON(!RB_EMPTY_ROOT(&bfqq->sort_list) && !entity->on_st);
	BUG_ON(bfq_bfqq_busy(bfqq) && RB_EMPTY_ROOT(&bfqq->sort_list)
	       && entity->on_st &&
	       bfqq != bfqd->in_service_queue);
	BUG_ON(!bfq_bfqq_busy(bfqq) && bfqq == bfqd->in_service_queue);

	/* If bfqq is empty, then bfq_bfqq_expire also invokes
	 * bfq_del_bfqq_busy, thereby removing bfqq and its entity
	 * from data structures related to current group. Otherwise we
	 * need to remove bfqq explicitly with bfq_deactivate_bfqq, as
	 * we do below.
	 */
	if (bfqq == bfqd->in_service_queue)
		bfq_bfqq_expire(bfqd, bfqd->in_service_queue,
				false, BFQ_BFQQ_PREEMPTED);

	BUG_ON(entity->on_st && !bfq_bfqq_busy(bfqq)
	    && &bfq_entity_service_tree(entity)->idle !=
	       entity->tree);

	BUG_ON(RB_EMPTY_ROOT(&bfqq->sort_list) && bfq_bfqq_busy(bfqq));

	if (bfq_bfqq_busy(bfqq))
		bfq_deactivate_bfqq(bfqd, bfqq, false, false);
	else if (entity->on_st) {
		BUG_ON(&bfq_entity_service_tree(entity)->idle !=
		       entity->tree);
		bfq_put_idle_entity(bfq_entity_service_tree(entity), entity);
	}
	bfqg_put(bfqq_group(bfqq));

	/*
	 * Here we use a reference to bfqg.  We don't need a refcounter
	 * as the cgroup reference will not be dropped, so that its
	 * destroy() callback will not be invoked.
	 */
	entity->parent = bfqg->my_entity;
	entity->sched_data = &bfqg->sched_data;
	bfqg_get(bfqg);

	BUG_ON(RB_EMPTY_ROOT(&bfqq->sort_list) && bfq_bfqq_busy(bfqq));
	if (bfq_bfqq_busy(bfqq)) {
		bfq_pos_tree_add_move(bfqd, bfqq);
		bfq_activate_bfqq(bfqd, bfqq);
	}

	if (!bfqd->in_service_queue && !bfqd->rq_in_driver)
		bfq_schedule_dispatch(bfqd);
	BUG_ON(entity->on_st && !bfq_bfqq_busy(bfqq)
	       && &bfq_entity_service_tree(entity)->idle !=
	       entity->tree);
}

/**
 * __bfq_bic_change_cgroup - move @bic to @cgroup.
 * @bfqd: the queue descriptor.
 * @bic: the bic to move.
 * @blkcg: the blk-cgroup to move to.
 *
 * Move bic to blkcg, assuming that bfqd->queue is locked; the caller
 * has to make sure that the reference to cgroup is valid across the call.
 *
 * NOTE: an alternative approach might have been to store the current
 * cgroup in bfqq and getting a reference to it, reducing the lookup
 * time here, at the price of slightly more complex code.
 */
static struct bfq_group *__bfq_bic_change_cgroup(struct bfq_data *bfqd,
						struct bfq_io_cq *bic,
						struct blkcg *blkcg)
{
	struct bfq_queue *async_bfqq = bic_to_bfqq(bic, 0);
	struct bfq_queue *sync_bfqq = bic_to_bfqq(bic, 1);
	struct bfq_group *bfqg;
	struct bfq_entity *entity;

	lockdep_assert_held(bfqd->queue->queue_lock);

	bfqg = bfq_find_set_group(bfqd, blkcg);

	if (unlikely(!bfqg))
		bfqg = bfqd->root_group;

	if (async_bfqq) {
		entity = &async_bfqq->entity;

		if (entity->sched_data != &bfqg->sched_data) {
			bic_set_bfqq(bic, NULL, 0);
			bfq_log_bfqq(bfqd, async_bfqq,
				     "bic_change_group: %p %d",
				     async_bfqq,
				     async_bfqq->ref);
			bfq_put_queue(async_bfqq);
		}
	}

	if (sync_bfqq) {
		entity = &sync_bfqq->entity;
		if (entity->sched_data != &bfqg->sched_data)
			bfq_bfqq_move(bfqd, sync_bfqq, bfqg);
	}

	return bfqg;
}

static void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio)
{
	struct bfq_data *bfqd = bic_to_bfqd(bic);
	struct bfq_group *bfqg = NULL;
	uint64_t serial_nr;

	rcu_read_lock();
	serial_nr = bio_blkcg(bio)->css.serial_nr;

	/*
	 * Check whether blkcg has changed.  The condition may trigger
	 * spuriously on a newly created cic but there's no harm.
	 */
	if (unlikely(!bfqd) || likely(bic->blkcg_serial_nr == serial_nr))
		goto out;

	bfqg = __bfq_bic_change_cgroup(bfqd, bic, bio_blkcg(bio));
	bic->blkcg_serial_nr = serial_nr;
out:
	rcu_read_unlock();
}

/**
 * bfq_flush_idle_tree - deactivate any entity on the idle tree of @st.
 * @st: the service tree being flushed.
 */
static void bfq_flush_idle_tree(struct bfq_service_tree *st)
{
	struct bfq_entity *entity = st->first_idle;

	for (; entity ; entity = st->first_idle)
		__bfq_deactivate_entity(entity, false);
}

/**
 * bfq_reparent_leaf_entity - move leaf entity to the root_group.
 * @bfqd: the device data structure with the root group.
 * @entity: the entity to move.
 */
static void bfq_reparent_leaf_entity(struct bfq_data *bfqd,
				     struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	BUG_ON(!bfqq);
	bfq_bfqq_move(bfqd, bfqq, bfqd->root_group);
	return;
}

/**
 * bfq_reparent_active_entities - move to the root group all active
 *                                entities.
 * @bfqd: the device data structure with the root group.
 * @bfqg: the group to move from.
 * @st: the service tree with the entities.
 *
 * Needs queue_lock to be taken and reference to be valid over the call.
 */
static void bfq_reparent_active_entities(struct bfq_data *bfqd,
					 struct bfq_group *bfqg,
					 struct bfq_service_tree *st)
{
	struct rb_root *active = &st->active;
	struct bfq_entity *entity = NULL;

	if (!RB_EMPTY_ROOT(&st->active))
		entity = bfq_entity_of(rb_first(active));

	for (; entity ; entity = bfq_entity_of(rb_first(active)))
		bfq_reparent_leaf_entity(bfqd, entity);

	if (bfqg->sched_data.in_service_entity)
		bfq_reparent_leaf_entity(bfqd,
			bfqg->sched_data.in_service_entity);

	return;
}

/**
 * bfq_pd_offline - deactivate the entity associated with @pd,
 *		    and reparent its children entities.
 * @pd: descriptor of the policy going offline.
 *
 * blkio already grabs the queue_lock for us, so no need to use
 * RCU-based magic
 */
static void bfq_pd_offline(struct blkg_policy_data *pd)
{
	struct bfq_service_tree *st;
	struct bfq_group *bfqg;
	struct bfq_data *bfqd;
	struct bfq_entity *entity;
	int i;

	BUG_ON(!pd);
	bfqg = pd_to_bfqg(pd);
	BUG_ON(!bfqg);
	bfqd = bfqg->bfqd;
	BUG_ON(bfqd && !bfqd->root_group);

	entity = bfqg->my_entity;

	if (!entity) /* root group */
		return;

	/*
	 * Empty all service_trees belonging to this group before
	 * deactivating the group itself.
	 */
	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++) {
		st = bfqg->sched_data.service_tree + i;
		/*
		 * The idle tree may still contain bfq_queues belonging
		 * to exited task because they never migrated to a different
		 * cgroup from the one being destroyed now.  No one else
		 * can access them so it's safe to act without any lock.
		 */
		bfq_flush_idle_tree(st);

		/*
		 * It may happen that some queues are still active
		 * (busy) upon group destruction (if the corresponding
		 * processes have been forced to terminate). We move
		 * all the leaf entities corresponding to these queues
		 * to the root_group.
		 * Also, it may happen that the group has an entity
		 * in service, which is disconnected from the active
		 * tree: it must be moved, too.
		 * There is no need to put the sync queues, as the
		 * scheduler has taken no reference.
		 */
		bfq_reparent_active_entities(bfqd, bfqg, st);
		BUG_ON(!RB_EMPTY_ROOT(&st->active));
		BUG_ON(!RB_EMPTY_ROOT(&st->idle));
	}
	BUG_ON(bfqg->sched_data.next_in_service);
	BUG_ON(bfqg->sched_data.in_service_entity);

	__bfq_deactivate_entity(entity, false);
	bfq_put_async_queues(bfqd, bfqg);

	/*
	 * @blkg is going offline and will be ignored by
	 * blkg_[rw]stat_recursive_sum().  Transfer stats to the parent so
	 * that they don't get lost.  If IOs complete after this point, the
	 * stats for them will be lost.  Oh well...
	 */
	bfqg_stats_xfer_dead(bfqg);
}

static void bfq_end_wr_async(struct bfq_data *bfqd)
{
	struct blkcg_gq *blkg;

	list_for_each_entry(blkg, &bfqd->queue->blkg_list, q_node) {
		struct bfq_group *bfqg = blkg_to_bfqg(blkg);
		BUG_ON(!bfqg);

		bfq_end_wr_async_queues(bfqd, bfqg);
	}
	bfq_end_wr_async_queues(bfqd, bfqd->root_group);
}

static int bfq_io_show_weight(struct seq_file *sf, void *v)
{
	struct blkcg *blkcg = css_to_blkcg(seq_css(sf));
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	unsigned int val = 0;

	if (bfqgd)
		val = bfqgd->weight;

	seq_printf(sf, "%u\n", val);

	return 0;
}

static int bfq_io_set_weight_legacy(struct cgroup_subsys_state *css,
				    struct cftype *cftype,
				    u64 val)
{
	struct blkcg *blkcg = css_to_blkcg(css);
	struct bfq_group_data *bfqgd = blkcg_to_bfqgd(blkcg);
	struct blkcg_gq *blkg;
	int ret = -ERANGE;

	if (val < BFQ_MIN_WEIGHT || val > BFQ_MAX_WEIGHT)
		return ret;

	ret = 0;
	spin_lock_irq(&blkcg->lock);
	bfqgd->weight = (unsigned short)val;
	hlist_for_each_entry(blkg, &blkcg->blkg_list, blkcg_node) {
		struct bfq_group *bfqg = blkg_to_bfqg(blkg);
		if (!bfqg)
			continue;
		/*
		 * Setting the prio_changed flag of the entity
		 * to 1 with new_weight == weight would re-set
		 * the value of the weight to its ioprio mapping.
		 * Set the flag only if necessary.
		 */
		if ((unsigned short)val != bfqg->entity.new_weight) {
			bfqg->entity.new_weight = (unsigned short)val;
			/*
			 * Make sure that the above new value has been
			 * stored in bfqg->entity.new_weight before
			 * setting the prio_changed flag. In fact,
			 * this flag may be read asynchronously (in
			 * critical sections protected by a different
			 * lock than that held here), and finding this
			 * flag set may cause the execution of the code
			 * for updating parameters whose value may
			 * depend also on bfqg->entity.new_weight (in
			 * __bfq_entity_update_weight_prio).
			 * This barrier makes sure that the new value
			 * of bfqg->entity.new_weight is correctly
			 * seen in that code.
			 */
			smp_wmb();
			bfqg->entity.prio_changed = 1;
		}
	}
	spin_unlock_irq(&blkcg->lock);

	return ret;
}

static ssize_t bfq_io_set_weight(struct kernfs_open_file *of,
				 char *buf, size_t nbytes,
				 loff_t off)
{
	u64 weight;
	/* First unsigned long found in the file is used */
	int ret = kstrtoull(strim(buf), 0, &weight);

	if (ret)
		return ret;

	return bfq_io_set_weight_legacy(of_css(of), NULL, weight);
}

static int bfqg_print_stat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), blkg_prfill_stat,
			  &blkcg_policy_bfq, seq_cft(sf)->private, false);
	return 0;
}

static int bfqg_print_rwstat(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)), blkg_prfill_rwstat,
			  &blkcg_policy_bfq, seq_cft(sf)->private, true);
	return 0;
}

static u64 bfqg_prfill_stat_recursive(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	u64 sum = blkg_stat_recursive_sum(pd_to_blkg(pd),
					  &blkcg_policy_bfq, off);
	return __blkg_prfill_u64(sf, pd, sum);
}

static u64 bfqg_prfill_rwstat_recursive(struct seq_file *sf,
					struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat sum = blkg_rwstat_recursive_sum(pd_to_blkg(pd),
							   &blkcg_policy_bfq,
							   off);
	return __blkg_prfill_rwstat(sf, pd, &sum);
}

static int bfqg_print_stat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_stat_recursive, &blkcg_policy_bfq,
			  seq_cft(sf)->private, false);
	return 0;
}

static int bfqg_print_rwstat_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_rwstat_recursive, &blkcg_policy_bfq,
			  seq_cft(sf)->private, true);
	return 0;
}

static u64 bfqg_prfill_sectors(struct seq_file *sf, struct blkg_policy_data *pd,
			       int off)
{
	u64 sum = blkg_rwstat_total(&pd->blkg->stat_bytes);

	return __blkg_prfill_u64(sf, pd, sum >> 9);
}

static int bfqg_print_stat_sectors(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_sectors, &blkcg_policy_bfq, 0, false);
	return 0;
}

static u64 bfqg_prfill_sectors_recursive(struct seq_file *sf,
					 struct blkg_policy_data *pd, int off)
{
	struct blkg_rwstat tmp = blkg_rwstat_recursive_sum(pd->blkg, NULL,
					offsetof(struct blkcg_gq, stat_bytes));
	u64 sum = atomic64_read(&tmp.aux_cnt[BLKG_RWSTAT_READ]) +
		atomic64_read(&tmp.aux_cnt[BLKG_RWSTAT_WRITE]);

	return __blkg_prfill_u64(sf, pd, sum >> 9);
}

static int bfqg_print_stat_sectors_recursive(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_sectors_recursive, &blkcg_policy_bfq, 0,
			  false);
	return 0;
}


static u64 bfqg_prfill_avg_queue_size(struct seq_file *sf,
				      struct blkg_policy_data *pd, int off)
{
	struct bfq_group *bfqg = pd_to_bfqg(pd);
	u64 samples = blkg_stat_read(&bfqg->stats.avg_queue_size_samples);
	u64 v = 0;

	if (samples) {
		v = blkg_stat_read(&bfqg->stats.avg_queue_size_sum);
		v = div64_u64(v, samples);
	}
	__blkg_prfill_u64(sf, pd, v);
	return 0;
}

/* print avg_queue_size */
static int bfqg_print_avg_queue_size(struct seq_file *sf, void *v)
{
	blkcg_print_blkgs(sf, css_to_blkcg(seq_css(sf)),
			  bfqg_prfill_avg_queue_size, &blkcg_policy_bfq,
			  0, false);
	return 0;
}

static struct bfq_group *bfq_create_group_hierarchy(struct bfq_data *bfqd, int node)
{
	int ret;

	ret = blkcg_activate_policy(bfqd->queue, &blkcg_policy_bfq);
	if (ret)
		return NULL;

        return blkg_to_bfqg(bfqd->queue->root_blkg);
}

static struct cftype bfq_blkcg_legacy_files[] = {
	{
		.name = "bfq.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight,
		.write_u64 = bfq_io_set_weight_legacy,
	},
	/* statistics, covers only the tasks in the bfqg */
	{
		.name = "bfq.time",
		.private = offsetof(struct bfq_group, stats.time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.sectors",
		.seq_show = bfqg_print_stat_sectors,
	},
	{
		.name = "bfq.io_service_bytes",
		.private = (unsigned long)&blkcg_policy_bfq,
		.seq_show = blkg_print_stat_bytes,
	},
	{
		.name = "bfq.io_serviced",
		.private = (unsigned long)&blkcg_policy_bfq,
		.seq_show = blkg_print_stat_ios,
	},
	{
		.name = "bfq.io_service_time",
		.private = offsetof(struct bfq_group, stats.service_time),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_wait_time",
		.private = offsetof(struct bfq_group, stats.wait_time),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_merged",
		.private = offsetof(struct bfq_group, stats.merged),
		.seq_show = bfqg_print_rwstat,
	},
	{
		.name = "bfq.io_queued",
		.private = offsetof(struct bfq_group, stats.queued),
		.seq_show = bfqg_print_rwstat,
	},

	/* the same statictics which cover the bfqg and its descendants */
	{
		.name = "bfq.time_recursive",
		.private = offsetof(struct bfq_group, stats.time),
		.seq_show = bfqg_print_stat_recursive,
	},
	{
		.name = "bfq.sectors_recursive",
		.seq_show = bfqg_print_stat_sectors_recursive,
	},
	{
		.name = "bfq.io_service_bytes_recursive",
		.private = (unsigned long)&blkcg_policy_bfq,
		.seq_show = blkg_print_stat_bytes_recursive,
	},
	{
		.name = "bfq.io_serviced_recursive",
		.private = (unsigned long)&blkcg_policy_bfq,
		.seq_show = blkg_print_stat_ios_recursive,
	},
	{
		.name = "bfq.io_service_time_recursive",
		.private = offsetof(struct bfq_group, stats.service_time),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_wait_time_recursive",
		.private = offsetof(struct bfq_group, stats.wait_time),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_merged_recursive",
		.private = offsetof(struct bfq_group, stats.merged),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.io_queued_recursive",
		.private = offsetof(struct bfq_group, stats.queued),
		.seq_show = bfqg_print_rwstat_recursive,
	},
	{
		.name = "bfq.avg_queue_size",
		.seq_show = bfqg_print_avg_queue_size,
	},
	{
		.name = "bfq.group_wait_time",
		.private = offsetof(struct bfq_group, stats.group_wait_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.idle_time",
		.private = offsetof(struct bfq_group, stats.idle_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.empty_time",
		.private = offsetof(struct bfq_group, stats.empty_time),
		.seq_show = bfqg_print_stat,
	},
	{
		.name = "bfq.dequeue",
		.private = offsetof(struct bfq_group, stats.dequeue),
		.seq_show = bfqg_print_stat,
	},
	{ }	/* terminate */
};

static struct cftype bfq_blkg_files[] = {
	{
		.name = "bfq.weight",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = bfq_io_show_weight,
		.write = bfq_io_set_weight,
	},
	{} /* terminate */
};

#else /* CONFIG_BFQ_GROUP_IOSCHED */

static inline void bfqg_stats_update_io_add(struct bfq_group *bfqg,
			struct bfq_queue *bfqq, int op, int op_flags) { }
static inline void bfqg_stats_update_io_remove(struct bfq_group *bfqg,
					       int op, int op_flags) { }
static inline void bfqg_stats_update_io_merged(struct bfq_group *bfqg,
					       int op, int op_flags) { }
static inline void bfqg_stats_update_completion(struct bfq_group *bfqg,
			uint64_t start_time, uint64_t io_start_time, int op, int op_flags) { }
static inline void
bfqg_stats_set_start_group_wait_time(struct bfq_group *bfqg,
				     struct bfq_group *curr_bfqg) { }
static inline void bfqg_stats_end_empty_time(struct bfqg_stats *stats) { }
static inline void bfqg_stats_update_dequeue(struct bfq_group *bfqg) { }
static inline void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg) { }
static inline void bfqg_stats_update_idle_time(struct bfq_group *bfqg) { }
static inline void bfqg_stats_set_start_idle_time(struct bfq_group *bfqg) { }
static inline void bfqg_stats_update_avg_queue_size(struct bfq_group *bfqg) { }

static void bfq_bfqq_move(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  struct bfq_group *bfqg) {}

static void bfq_init_entity(struct bfq_entity *entity,
			    struct bfq_group *bfqg)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	entity->weight = entity->new_weight;
	entity->orig_weight = entity->new_weight;
	if (bfqq) {
		bfqq->ioprio = bfqq->new_ioprio;
		bfqq->ioprio_class = bfqq->new_ioprio_class;
	}
	entity->sched_data = &bfqg->sched_data;
}

static void bfq_bic_update_cgroup(struct bfq_io_cq *bic, struct bio *bio) {}

static void bfq_end_wr_async(struct bfq_data *bfqd)
{
	bfq_end_wr_async_queues(bfqd, bfqd->root_group);
}

static struct bfq_group *bfq_find_set_group(struct bfq_data *bfqd,
					    struct blkcg *blkcg)
{
	return bfqd->root_group;
}

static struct bfq_group *bfqq_group(struct bfq_queue *bfqq)
{
	return bfqq->bfqd->root_group;
}

static struct bfq_group *
bfq_create_group_hierarchy(struct bfq_data *bfqd, int node)
{
	struct bfq_group *bfqg;
	int i;

	bfqg = kmalloc_node(sizeof(*bfqg), GFP_KERNEL | __GFP_ZERO, node);
	if (!bfqg)
		return NULL;

	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++)
		bfqg->sched_data.service_tree[i] = BFQ_SERVICE_TREE_INIT;

	return bfqg;
}
#endif
