/*
 * net/tipc/name_table.c: TIPC name table code
 *
 * Copyright (c) 2000-2006, 2014-2018, Ericsson AB
 * Copyright (c) 2004-2008, 2010-2014, Wind River Systems
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <net/sock.h>
#include "core.h"
#include "netlink.h"
#include "name_table.h"
#include "name_distr.h"
#include "subscr.h"
#include "bcast.h"
#include "addr.h"
#include "node.h"
#include "group.h"
#include <net/genetlink.h>

#define TIPC_NAMETBL_SIZE 1024		/* must be a power of 2 */

/**
 * struct name_info - name sequence publication info
 * @node_list: list of publications on own node of this <type,lower,upper>
 * @all_publ: list of all publications of this <type,lower,upper>
 */
struct name_info {
	struct list_head local_publ;
	struct list_head all_publ;
};

/**
 * struct sub_seq - container for all published instances of a name sequence
 * @lower: name sequence lower bound
 * @upper: name sequence upper bound
 * @info: pointer to name sequence publication info
 */
struct sub_seq {
	u32 lower;
	u32 upper;
	struct name_info *info;
};

/**
 * struct name_seq - container for all published instances of a name type
 * @type: 32 bit 'type' value for name sequence
 * @sseq: pointer to dynamically-sized array of sub-sequences of this 'type';
 *        sub-sequences are sorted in ascending order
 * @alloc: number of sub-sequences currently in array
 * @first_free: array index of first unused sub-sequence entry
 * @ns_list: links to adjacent name sequences in hash chain
 * @subscriptions: list of subscriptions for this 'type'
 * @lock: spinlock controlling access to publication lists of all sub-sequences
 * @rcu: RCU callback head used for deferred freeing
 */
struct name_seq {
	u32 type;
	struct sub_seq *sseqs;
	u32 alloc;
	u32 first_free;
	struct hlist_node ns_list;
	struct list_head subscriptions;
	spinlock_t lock;
	struct rcu_head rcu;
};

static int hash(int x)
{
	return x & (TIPC_NAMETBL_SIZE - 1);
}

/**
 * publ_create - create a publication structure
 */
static struct publication *publ_create(u32 type, u32 lower, u32 upper,
				       u32 scope, u32 node, u32 port,
				       u32 key)
{
	struct publication *publ = kzalloc(sizeof(*publ), GFP_ATOMIC);
	if (publ == NULL) {
		pr_warn("Publication creation failure, no memory\n");
		return NULL;
	}

	publ->type = type;
	publ->lower = lower;
	publ->upper = upper;
	publ->scope = scope;
	publ->node = node;
	publ->port = port;
	publ->key = key;
	INIT_LIST_HEAD(&publ->binding_sock);
	return publ;
}

/**
 * tipc_subseq_alloc - allocate a specified number of sub-sequence structures
 */
static struct sub_seq *tipc_subseq_alloc(u32 cnt)
{
	return kcalloc(cnt, sizeof(struct sub_seq), GFP_ATOMIC);
}

/**
 * tipc_nameseq_create - create a name sequence structure for the specified 'type'
 *
 * Allocates a single sub-sequence structure and sets it to all 0's.
 */
static struct name_seq *tipc_nameseq_create(u32 type, struct hlist_head *seq_head)
{
	struct name_seq *nseq = kzalloc(sizeof(*nseq), GFP_ATOMIC);
	struct sub_seq *sseq = tipc_subseq_alloc(1);

	if (!nseq || !sseq) {
		pr_warn("Name sequence creation failed, no memory\n");
		kfree(nseq);
		kfree(sseq);
		return NULL;
	}

	spin_lock_init(&nseq->lock);
	nseq->type = type;
	nseq->sseqs = sseq;
	nseq->alloc = 1;
	INIT_HLIST_NODE(&nseq->ns_list);
	INIT_LIST_HEAD(&nseq->subscriptions);
	hlist_add_head_rcu(&nseq->ns_list, seq_head);
	return nseq;
}

/**
 * nameseq_find_subseq - find sub-sequence (if any) matching a name instance
 *
 * Very time-critical, so binary searches through sub-sequence array.
 */
static struct sub_seq *nameseq_find_subseq(struct name_seq *nseq,
					   u32 instance)
{
	struct sub_seq *sseqs = nseq->sseqs;
	int low = 0;
	int high = nseq->first_free - 1;
	int mid;

	while (low <= high) {
		mid = (low + high) / 2;
		if (instance < sseqs[mid].lower)
			high = mid - 1;
		else if (instance > sseqs[mid].upper)
			low = mid + 1;
		else
			return &sseqs[mid];
	}
	return NULL;
}

/**
 * nameseq_locate_subseq - determine position of name instance in sub-sequence
 *
 * Returns index in sub-sequence array of the entry that contains the specified
 * instance value; if no entry contains that value, returns the position
 * where a new entry for it would be inserted in the array.
 *
 * Note: Similar to binary search code for locating a sub-sequence.
 */
static u32 nameseq_locate_subseq(struct name_seq *nseq, u32 instance)
{
	struct sub_seq *sseqs = nseq->sseqs;
	int low = 0;
	int high = nseq->first_free - 1;
	int mid;

	while (low <= high) {
		mid = (low + high) / 2;
		if (instance < sseqs[mid].lower)
			high = mid - 1;
		else if (instance > sseqs[mid].upper)
			low = mid + 1;
		else
			return mid;
	}
	return low;
}

/**
 * tipc_nameseq_insert_publ
 */
static struct publication *tipc_nameseq_insert_publ(struct net *net,
						    struct name_seq *nseq,
						    u32 type, u32 lower,
						    u32 upper, u32 scope,
						    u32 node, u32 port, u32 key)
{
	struct tipc_subscription *s;
	struct tipc_subscription *st;
	struct publication *publ;
	struct sub_seq *sseq;
	struct name_info *info;
	int created_subseq = 0;

	sseq = nameseq_find_subseq(nseq, lower);
	if (sseq) {

		/* Lower end overlaps existing entry => need an exact match */
		if ((sseq->lower != lower) || (sseq->upper != upper)) {
			return NULL;
		}

		info = sseq->info;

		/* Check if an identical publication already exists */
		list_for_each_entry(publ, &info->all_publ, all_publ) {
			if (publ->port == port && publ->key == key &&
			    (!publ->node || publ->node == node))
				return NULL;
		}
	} else {
		u32 inspos;
		struct sub_seq *freesseq;

		/* Find where lower end should be inserted */
		inspos = nameseq_locate_subseq(nseq, lower);

		/* Fail if upper end overlaps into an existing entry */
		if ((inspos < nseq->first_free) &&
		    (upper >= nseq->sseqs[inspos].lower)) {
			return NULL;
		}

		/* Ensure there is space for new sub-sequence */
		if (nseq->first_free == nseq->alloc) {
			struct sub_seq *sseqs = tipc_subseq_alloc(nseq->alloc * 2);

			if (!sseqs) {
				pr_warn("Cannot publish {%u,%u,%u}, no memory\n",
					type, lower, upper);
				return NULL;
			}
			memcpy(sseqs, nseq->sseqs,
			       nseq->alloc * sizeof(struct sub_seq));
			kfree(nseq->sseqs);
			nseq->sseqs = sseqs;
			nseq->alloc *= 2;
		}

		info = kzalloc(sizeof(*info), GFP_ATOMIC);
		if (!info) {
			pr_warn("Cannot publish {%u,%u,%u}, no memory\n",
				type, lower, upper);
			return NULL;
		}

		INIT_LIST_HEAD(&info->local_publ);
		INIT_LIST_HEAD(&info->all_publ);

		/* Insert new sub-sequence */
		sseq = &nseq->sseqs[inspos];
		freesseq = &nseq->sseqs[nseq->first_free];
		memmove(sseq + 1, sseq, (freesseq - sseq) * sizeof(*sseq));
		memset(sseq, 0, sizeof(*sseq));
		nseq->first_free++;
		sseq->lower = lower;
		sseq->upper = upper;
		sseq->info = info;
		created_subseq = 1;
	}

	/* Insert a publication */
	publ = publ_create(type, lower, upper, scope, node, port, key);
	if (!publ)
		return NULL;

	list_add(&publ->all_publ, &info->all_publ);

	if (in_own_node(net, node))
		list_add(&publ->local_publ, &info->local_publ);

	/* Any subscriptions waiting for notification?  */
	list_for_each_entry_safe(s, st, &nseq->subscriptions, nameseq_list) {
		tipc_sub_report_overlap(s, publ->lower, publ->upper,
					TIPC_PUBLISHED, publ->port,
					publ->node, publ->scope,
					created_subseq);
	}
	return publ;
}

/**
 * tipc_nameseq_remove_publ
 *
 * NOTE: There may be cases where TIPC is asked to remove a publication
 * that is not in the name table.  For example, if another node issues a
 * publication for a name sequence that overlaps an existing name sequence
 * the publication will not be recorded, which means the publication won't
 * be found when the name sequence is later withdrawn by that node.
 * A failed withdraw request simply returns a failure indication and lets the
 * caller issue any error or warning messages associated with such a problem.
 */
static struct publication *tipc_nameseq_remove_publ(struct net *net,
						    struct name_seq *nseq,
						    u32 inst, u32 node,
						    u32 port, u32 key)
{
	struct publication *publ;
	struct sub_seq *sseq = nameseq_find_subseq(nseq, inst);
	struct name_info *info;
	struct sub_seq *free;
	struct tipc_subscription *s, *st;
	int removed_subseq = 0;

	if (!sseq)
		return NULL;

	info = sseq->info;

	/* Locate publication, if it exists */
	list_for_each_entry(publ, &info->all_publ, all_publ) {
		if (publ->key == key && publ->port == port &&
		    (!publ->node || publ->node == node))
			goto found;
	}
	return NULL;

found:
	list_del(&publ->all_publ);
	if (in_own_node(net, node))
		list_del(&publ->local_publ);

	/* Contract subseq list if no more publications for that subseq */
	if (list_empty(&info->all_publ)) {
		kfree(info);
		free = &nseq->sseqs[nseq->first_free--];
		memmove(sseq, sseq + 1, (free - (sseq + 1)) * sizeof(*sseq));
		removed_subseq = 1;
	}

	/* Notify any waiting subscriptions */
	list_for_each_entry_safe(s, st, &nseq->subscriptions, nameseq_list) {
		tipc_sub_report_overlap(s, publ->lower, publ->upper,
					TIPC_WITHDRAWN, publ->port,
					publ->node, publ->scope,
					removed_subseq);
	}

	return publ;
}

/**
 * tipc_nameseq_subscribe - attach a subscription, and optionally
 * issue the prescribed number of events if there is any sub-
 * sequence overlapping with the requested sequence
 */
static void tipc_nameseq_subscribe(struct name_seq *nseq,
				   struct tipc_subscription *sub)
{
	struct sub_seq *sseq = nseq->sseqs;
	struct tipc_name_seq ns;
	struct tipc_subscr *s = &sub->evt.s;
	bool no_status;

	ns.type = tipc_sub_read(s, seq.type);
	ns.lower = tipc_sub_read(s, seq.lower);
	ns.upper = tipc_sub_read(s, seq.upper);
	no_status = tipc_sub_read(s, filter) & TIPC_SUB_NO_STATUS;

	tipc_sub_get(sub);
	list_add(&sub->nameseq_list, &nseq->subscriptions);

	if (no_status || !sseq)
		return;

	while (sseq != &nseq->sseqs[nseq->first_free]) {
		if (tipc_sub_check_overlap(&ns, sseq->lower, sseq->upper)) {
			struct publication *crs;
			struct name_info *info = sseq->info;
			int must_report = 1;

			list_for_each_entry(crs, &info->all_publ, all_publ) {
				tipc_sub_report_overlap(sub, sseq->lower,
							sseq->upper,
							TIPC_PUBLISHED,
							crs->port,
							crs->node,
							crs->scope,
							must_report);
				must_report = 0;
			}
		}
		sseq++;
	}
}

static struct name_seq *nametbl_find_seq(struct net *net, u32 type)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct hlist_head *seq_head;
	struct name_seq *ns;

	seq_head = &tn->nametbl->seq_hlist[hash(type)];
	hlist_for_each_entry_rcu(ns, seq_head, ns_list) {
		if (ns->type == type)
			return ns;
	}

	return NULL;
};

struct publication *tipc_nametbl_insert_publ(struct net *net, u32 type,
					     u32 lower, u32 upper, u32 scope,
					     u32 node, u32 port, u32 key)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct publication *publ;
	struct name_seq *seq = nametbl_find_seq(net, type);
	int index = hash(type);

	if (scope > TIPC_NODE_SCOPE || lower > upper) {
		pr_debug("Failed to publish illegal {%u,%u,%u} with scope %u\n",
			 type, lower, upper, scope);
		return NULL;
	}

	if (!seq)
		seq = tipc_nameseq_create(type, &tn->nametbl->seq_hlist[index]);
	if (!seq)
		return NULL;

	spin_lock_bh(&seq->lock);
	publ = tipc_nameseq_insert_publ(net, seq, type, lower, upper,
					scope, node, port, key);
	spin_unlock_bh(&seq->lock);
	return publ;
}

struct publication *tipc_nametbl_remove_publ(struct net *net, u32 type,
					     u32 lower, u32 node, u32 port,
					     u32 key)
{
	struct publication *publ;
	struct name_seq *seq = nametbl_find_seq(net, type);

	if (!seq)
		return NULL;

	spin_lock_bh(&seq->lock);
	publ = tipc_nameseq_remove_publ(net, seq, lower, node, port, key);
	if (!seq->first_free && list_empty(&seq->subscriptions)) {
		hlist_del_init_rcu(&seq->ns_list);
		kfree(seq->sseqs);
		spin_unlock_bh(&seq->lock);
		kfree_rcu(seq, rcu);
		return publ;
	}
	spin_unlock_bh(&seq->lock);
	return publ;
}

/**
 * tipc_nametbl_translate - perform name translation
 *
 * On entry, 'destnode' is the search domain used during translation.
 *
 * On exit:
 * - if name translation is deferred to another node/cluster/zone,
 *   leaves 'destnode' unchanged (will be non-zero) and returns 0
 * - if name translation is attempted and succeeds, sets 'destnode'
 *   to publishing node and returns port reference (will be non-zero)
 * - if name translation is attempted and fails, sets 'destnode' to 0
 *   and returns 0
 */
u32 tipc_nametbl_translate(struct net *net, u32 type, u32 instance,
			   u32 *destnode)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct sub_seq *sseq;
	struct name_info *info;
	struct publication *publ;
	struct name_seq *seq;
	u32 port = 0;
	u32 node = 0;

	if (!tipc_in_scope(*destnode, tn->own_addr))
		return 0;

	rcu_read_lock();
	seq = nametbl_find_seq(net, type);
	if (unlikely(!seq))
		goto not_found;
	spin_lock_bh(&seq->lock);
	sseq = nameseq_find_subseq(seq, instance);
	if (unlikely(!sseq))
		goto no_match;
	info = sseq->info;

	/* Closest-First Algorithm */
	if (likely(!*destnode)) {
		if (!list_empty(&info->local_publ)) {
			publ = list_first_entry(&info->local_publ,
						struct publication,
						local_publ);
			list_move_tail(&publ->local_publ,
				       &info->local_publ);
		} else {
			publ = list_first_entry(&info->all_publ,
						struct publication,
						all_publ);
			list_move_tail(&publ->all_publ,
				       &info->all_publ);
		}
	}

	/* Round-Robin Algorithm */
	else if (*destnode == tn->own_addr) {
		if (list_empty(&info->local_publ))
			goto no_match;
		publ = list_first_entry(&info->local_publ, struct publication,
					local_publ);
		list_move_tail(&publ->local_publ, &info->local_publ);
	} else {
		publ = list_first_entry(&info->all_publ, struct publication,
					all_publ);
		list_move_tail(&publ->all_publ, &info->all_publ);
	}

	port = publ->port;
	node = publ->node;
no_match:
	spin_unlock_bh(&seq->lock);
not_found:
	rcu_read_unlock();
	*destnode = node;
	return port;
}

bool tipc_nametbl_lookup(struct net *net, u32 type, u32 instance, u32 scope,
			 struct list_head *dsts, int *dstcnt, u32 exclude,
			 bool all)
{
	u32 self = tipc_own_addr(net);
	struct publication *publ;
	struct name_info *info;
	struct name_seq *seq;
	struct sub_seq *sseq;

	*dstcnt = 0;
	rcu_read_lock();
	seq = nametbl_find_seq(net, type);
	if (unlikely(!seq))
		goto exit;
	spin_lock_bh(&seq->lock);
	sseq = nameseq_find_subseq(seq, instance);
	if (likely(sseq)) {
		info = sseq->info;
		list_for_each_entry(publ, &info->all_publ, all_publ) {
			if (publ->scope != scope)
				continue;
			if (publ->port == exclude && publ->node == self)
				continue;
			tipc_dest_push(dsts, publ->node, publ->port);
			(*dstcnt)++;
			if (all)
				continue;
			list_move_tail(&publ->all_publ, &info->all_publ);
			break;
		}
	}
	spin_unlock_bh(&seq->lock);
exit:
	rcu_read_unlock();
	return !list_empty(dsts);
}

void tipc_nametbl_mc_lookup(struct net *net, u32 type, u32 lower, u32 upper,
			    u32 scope, bool exact, struct list_head *dports)
{
	struct sub_seq *sseq_stop;
	struct name_info *info;
	struct publication *p;
	struct name_seq *seq;
	struct sub_seq *sseq;

	rcu_read_lock();
	seq = nametbl_find_seq(net, type);
	if (!seq)
		goto exit;

	spin_lock_bh(&seq->lock);
	sseq = seq->sseqs + nameseq_locate_subseq(seq, lower);
	sseq_stop = seq->sseqs + seq->first_free;
	for (; sseq != sseq_stop; sseq++) {
		if (sseq->lower > upper)
			break;
		info = sseq->info;
		list_for_each_entry(p, &info->local_publ, local_publ) {
			if (p->scope == scope || (!exact && p->scope < scope))
				tipc_dest_push(dports, 0, p->port);
		}
	}
	spin_unlock_bh(&seq->lock);
exit:
	rcu_read_unlock();
}

/* tipc_nametbl_lookup_dst_nodes - find broadcast destination nodes
 * - Creates list of nodes that overlap the given multicast address
 * - Determines if any node local ports overlap
 */
void tipc_nametbl_lookup_dst_nodes(struct net *net, u32 type, u32 lower,
				   u32 upper, struct tipc_nlist *nodes)
{
	struct sub_seq *sseq, *stop;
	struct publication *publ;
	struct name_info *info;
	struct name_seq *seq;

	rcu_read_lock();
	seq = nametbl_find_seq(net, type);
	if (!seq)
		goto exit;

	spin_lock_bh(&seq->lock);
	sseq = seq->sseqs + nameseq_locate_subseq(seq, lower);
	stop = seq->sseqs + seq->first_free;
	for (; sseq != stop && sseq->lower <= upper; sseq++) {
		info = sseq->info;
		list_for_each_entry(publ, &info->all_publ, all_publ) {
			tipc_nlist_add(nodes, publ->node);
		}
	}
	spin_unlock_bh(&seq->lock);
exit:
	rcu_read_unlock();
}

/* tipc_nametbl_build_group - build list of communication group members
 */
void tipc_nametbl_build_group(struct net *net, struct tipc_group *grp,
			      u32 type, u32 scope)
{
	struct sub_seq *sseq, *stop;
	struct name_info *info;
	struct publication *p;
	struct name_seq *seq;

	rcu_read_lock();
	seq = nametbl_find_seq(net, type);
	if (!seq)
		goto exit;

	spin_lock_bh(&seq->lock);
	sseq = seq->sseqs;
	stop = seq->sseqs + seq->first_free;
	for (; sseq != stop; sseq++) {
		info = sseq->info;
		list_for_each_entry(p, &info->all_publ, all_publ) {
			if (p->scope != scope)
				continue;
			tipc_group_add_member(grp, p->node, p->port, p->lower);
		}
	}
	spin_unlock_bh(&seq->lock);
exit:
	rcu_read_unlock();
}

/*
 * tipc_nametbl_publish - add name publication to network name tables
 */
struct publication *tipc_nametbl_publish(struct net *net, u32 type, u32 lower,
					 u32 upper, u32 scope, u32 port_ref,
					 u32 key)
{
	struct publication *publ;
	struct sk_buff *buf = NULL;
	struct tipc_net *tn = net_generic(net, tipc_net_id);

	spin_lock_bh(&tn->nametbl_lock);
	if (tn->nametbl->local_publ_count >= TIPC_MAX_PUBLICATIONS) {
		pr_warn("Publication failed, local publication limit reached (%u)\n",
			TIPC_MAX_PUBLICATIONS);
		spin_unlock_bh(&tn->nametbl_lock);
		return NULL;
	}

	publ = tipc_nametbl_insert_publ(net, type, lower, upper, scope,
					tn->own_addr, port_ref, key);
	if (likely(publ)) {
		tn->nametbl->local_publ_count++;
		buf = tipc_named_publish(net, publ);
		/* Any pending external events? */
		tipc_named_process_backlog(net);
	}
	spin_unlock_bh(&tn->nametbl_lock);

	if (buf)
		tipc_node_broadcast(net, buf);
	return publ;
}

/**
 * tipc_nametbl_withdraw - withdraw name publication from network name tables
 */
int tipc_nametbl_withdraw(struct net *net, u32 type, u32 lower, u32 port,
			  u32 key)
{
	struct publication *publ;
	struct sk_buff *skb = NULL;
	struct tipc_net *tn = net_generic(net, tipc_net_id);

	spin_lock_bh(&tn->nametbl_lock);
	publ = tipc_nametbl_remove_publ(net, type, lower, tn->own_addr,
					port, key);
	if (likely(publ)) {
		tn->nametbl->local_publ_count--;
		skb = tipc_named_withdraw(net, publ);
		/* Any pending external events? */
		tipc_named_process_backlog(net);
		list_del_init(&publ->binding_sock);
		kfree_rcu(publ, rcu);
	} else {
		pr_err("Unable to remove local publication\n"
		       "(type=%u, lower=%u, port=%u, key=%u)\n",
		       type, lower, port, key);
	}
	spin_unlock_bh(&tn->nametbl_lock);

	if (skb) {
		tipc_node_broadcast(net, skb);
		return 1;
	}
	return 0;
}

/**
 * tipc_nametbl_subscribe - add a subscription object to the name table
 */
void tipc_nametbl_subscribe(struct tipc_subscription *sub)
{
	struct tipc_net *tn = tipc_net(sub->net);
	struct tipc_subscr *s = &sub->evt.s;
	u32 type = tipc_sub_read(s, seq.type);
	int index = hash(type);
	struct name_seq *seq;
	struct tipc_name_seq ns;

	spin_lock_bh(&tn->nametbl_lock);
	seq = nametbl_find_seq(sub->net, type);
	if (!seq)
		seq = tipc_nameseq_create(type, &tn->nametbl->seq_hlist[index]);
	if (seq) {
		spin_lock_bh(&seq->lock);
		tipc_nameseq_subscribe(seq, sub);
		spin_unlock_bh(&seq->lock);
	} else {
		ns.type = tipc_sub_read(s, seq.type);
		ns.lower = tipc_sub_read(s, seq.lower);
		ns.upper = tipc_sub_read(s, seq.upper);
		pr_warn("Failed to create subscription for {%u,%u,%u}\n",
			ns.type, ns.lower, ns.upper);
	}
	spin_unlock_bh(&tn->nametbl_lock);
}

/**
 * tipc_nametbl_unsubscribe - remove a subscription object from name table
 */
void tipc_nametbl_unsubscribe(struct tipc_subscription *sub)
{
	struct tipc_subscr *s = &sub->evt.s;
	struct tipc_net *tn = tipc_net(sub->net);
	struct name_seq *seq;
	u32 type = tipc_sub_read(s, seq.type);

	spin_lock_bh(&tn->nametbl_lock);
	seq = nametbl_find_seq(sub->net, type);
	if (seq != NULL) {
		spin_lock_bh(&seq->lock);
		list_del_init(&sub->nameseq_list);
		tipc_sub_put(sub);
		if (!seq->first_free && list_empty(&seq->subscriptions)) {
			hlist_del_init_rcu(&seq->ns_list);
			kfree(seq->sseqs);
			spin_unlock_bh(&seq->lock);
			kfree_rcu(seq, rcu);
		} else {
			spin_unlock_bh(&seq->lock);
		}
	}
	spin_unlock_bh(&tn->nametbl_lock);
}

int tipc_nametbl_init(struct net *net)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct name_table *tipc_nametbl;
	int i;

	tipc_nametbl = kzalloc(sizeof(*tipc_nametbl), GFP_ATOMIC);
	if (!tipc_nametbl)
		return -ENOMEM;

	for (i = 0; i < TIPC_NAMETBL_SIZE; i++)
		INIT_HLIST_HEAD(&tipc_nametbl->seq_hlist[i]);

	INIT_LIST_HEAD(&tipc_nametbl->node_scope);
	INIT_LIST_HEAD(&tipc_nametbl->cluster_scope);
	tn->nametbl = tipc_nametbl;
	spin_lock_init(&tn->nametbl_lock);
	return 0;
}

/**
 * tipc_purge_publications - remove all publications for a given type
 *
 * tipc_nametbl_lock must be held when calling this function
 */
static void tipc_purge_publications(struct net *net, struct name_seq *seq)
{
	struct publication *publ, *safe;
	struct sub_seq *sseq;
	struct name_info *info;

	spin_lock_bh(&seq->lock);
	sseq = seq->sseqs;
	info = sseq->info;
	list_for_each_entry_safe(publ, safe, &info->all_publ, all_publ) {
		tipc_nameseq_remove_publ(net, seq, publ->lower, publ->node,
					 publ->port, publ->key);
		kfree_rcu(publ, rcu);
	}
	hlist_del_init_rcu(&seq->ns_list);
	kfree(seq->sseqs);
	spin_unlock_bh(&seq->lock);

	kfree_rcu(seq, rcu);
}

void tipc_nametbl_stop(struct net *net)
{
	u32 i;
	struct name_seq *seq;
	struct hlist_head *seq_head;
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct name_table *tipc_nametbl = tn->nametbl;

	/* Verify name table is empty and purge any lingering
	 * publications, then release the name table
	 */
	spin_lock_bh(&tn->nametbl_lock);
	for (i = 0; i < TIPC_NAMETBL_SIZE; i++) {
		if (hlist_empty(&tipc_nametbl->seq_hlist[i]))
			continue;
		seq_head = &tipc_nametbl->seq_hlist[i];
		hlist_for_each_entry_rcu(seq, seq_head, ns_list) {
			tipc_purge_publications(net, seq);
		}
	}
	spin_unlock_bh(&tn->nametbl_lock);

	synchronize_net();
	kfree(tipc_nametbl);

}

static int __tipc_nl_add_nametable_publ(struct tipc_nl_msg *msg,
					struct name_seq *seq,
					struct sub_seq *sseq, u32 *last_publ)
{
	void *hdr;
	struct nlattr *attrs;
	struct nlattr *publ;
	struct publication *p;

	if (*last_publ) {
		list_for_each_entry(p, &sseq->info->all_publ, all_publ)
			if (p->key == *last_publ)
				break;
		if (p->key != *last_publ)
			return -EPIPE;
	} else {
		p = list_first_entry(&sseq->info->all_publ, struct publication,
				     all_publ);
	}

	list_for_each_entry_from(p, &sseq->info->all_publ, all_publ) {
		*last_publ = p->key;

		hdr = genlmsg_put(msg->skb, msg->portid, msg->seq,
				  &tipc_genl_family, NLM_F_MULTI,
				  TIPC_NL_NAME_TABLE_GET);
		if (!hdr)
			return -EMSGSIZE;

		attrs = nla_nest_start(msg->skb, TIPC_NLA_NAME_TABLE);
		if (!attrs)
			goto msg_full;

		publ = nla_nest_start(msg->skb, TIPC_NLA_NAME_TABLE_PUBL);
		if (!publ)
			goto attr_msg_full;

		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_TYPE, seq->type))
			goto publ_msg_full;
		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_LOWER, sseq->lower))
			goto publ_msg_full;
		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_UPPER, sseq->upper))
			goto publ_msg_full;
		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_SCOPE, p->scope))
			goto publ_msg_full;
		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_NODE, p->node))
			goto publ_msg_full;
		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_REF, p->port))
			goto publ_msg_full;
		if (nla_put_u32(msg->skb, TIPC_NLA_PUBL_KEY, p->key))
			goto publ_msg_full;

		nla_nest_end(msg->skb, publ);
		nla_nest_end(msg->skb, attrs);
		genlmsg_end(msg->skb, hdr);
	}
	*last_publ = 0;

	return 0;

publ_msg_full:
	nla_nest_cancel(msg->skb, publ);
attr_msg_full:
	nla_nest_cancel(msg->skb, attrs);
msg_full:
	genlmsg_cancel(msg->skb, hdr);

	return -EMSGSIZE;
}

static int __tipc_nl_subseq_list(struct tipc_nl_msg *msg, struct name_seq *seq,
				 u32 *last_lower, u32 *last_publ)
{
	struct sub_seq *sseq;
	struct sub_seq *sseq_start;
	int err;

	if (*last_lower) {
		sseq_start = nameseq_find_subseq(seq, *last_lower);
		if (!sseq_start)
			return -EPIPE;
	} else {
		sseq_start = seq->sseqs;
	}

	for (sseq = sseq_start; sseq != &seq->sseqs[seq->first_free]; sseq++) {
		err = __tipc_nl_add_nametable_publ(msg, seq, sseq, last_publ);
		if (err) {
			*last_lower = sseq->lower;
			return err;
		}
	}
	*last_lower = 0;

	return 0;
}

static int tipc_nl_seq_list(struct net *net, struct tipc_nl_msg *msg,
			    u32 *last_type, u32 *last_lower, u32 *last_publ)
{
	struct tipc_net *tn = net_generic(net, tipc_net_id);
	struct hlist_head *seq_head;
	struct name_seq *seq = NULL;
	int err;
	int i;

	if (*last_type)
		i = hash(*last_type);
	else
		i = 0;

	for (; i < TIPC_NAMETBL_SIZE; i++) {
		seq_head = &tn->nametbl->seq_hlist[i];

		if (*last_type) {
			seq = nametbl_find_seq(net, *last_type);
			if (!seq)
				return -EPIPE;
		} else {
			hlist_for_each_entry_rcu(seq, seq_head, ns_list)
				break;
			if (!seq)
				continue;
		}

		hlist_for_each_entry_from_rcu(seq, ns_list) {
			spin_lock_bh(&seq->lock);
			err = __tipc_nl_subseq_list(msg, seq, last_lower,
						    last_publ);

			if (err) {
				*last_type = seq->type;
				spin_unlock_bh(&seq->lock);
				return err;
			}
			spin_unlock_bh(&seq->lock);
		}
		*last_type = 0;
	}
	return 0;
}

int tipc_nl_name_table_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	int err;
	int done = cb->args[3];
	u32 last_type = cb->args[0];
	u32 last_lower = cb->args[1];
	u32 last_publ = cb->args[2];
	struct net *net = sock_net(skb->sk);
	struct tipc_nl_msg msg;

	if (done)
		return 0;

	msg.skb = skb;
	msg.portid = NETLINK_CB(cb->skb).portid;
	msg.seq = cb->nlh->nlmsg_seq;

	rcu_read_lock();
	err = tipc_nl_seq_list(net, &msg, &last_type, &last_lower, &last_publ);
	if (!err) {
		done = 1;
	} else if (err != -EMSGSIZE) {
		/* We never set seq or call nl_dump_check_consistent() this
		 * means that setting prev_seq here will cause the consistence
		 * check to fail in the netlink callback handler. Resulting in
		 * the NLMSG_DONE message having the NLM_F_DUMP_INTR flag set if
		 * we got an error.
		 */
		cb->prev_seq = 1;
	}
	rcu_read_unlock();

	cb->args[0] = last_type;
	cb->args[1] = last_lower;
	cb->args[2] = last_publ;
	cb->args[3] = done;

	return skb->len;
}

struct tipc_dest *tipc_dest_find(struct list_head *l, u32 node, u32 port)
{
	u64 value = (u64)node << 32 | port;
	struct tipc_dest *dst;

	list_for_each_entry(dst, l, list) {
		if (dst->value != value)
			continue;
		return dst;
	}
	return NULL;
}

bool tipc_dest_push(struct list_head *l, u32 node, u32 port)
{
	u64 value = (u64)node << 32 | port;
	struct tipc_dest *dst;

	if (tipc_dest_find(l, node, port))
		return false;

	dst = kmalloc(sizeof(*dst), GFP_ATOMIC);
	if (unlikely(!dst))
		return false;
	dst->value = value;
	list_add(&dst->list, l);
	return true;
}

bool tipc_dest_pop(struct list_head *l, u32 *node, u32 *port)
{
	struct tipc_dest *dst;

	if (list_empty(l))
		return false;
	dst = list_first_entry(l, typeof(*dst), list);
	if (port)
		*port = dst->port;
	if (node)
		*node = dst->node;
	list_del(&dst->list);
	kfree(dst);
	return true;
}

bool tipc_dest_del(struct list_head *l, u32 node, u32 port)
{
	struct tipc_dest *dst;

	dst = tipc_dest_find(l, node, port);
	if (!dst)
		return false;
	list_del(&dst->list);
	kfree(dst);
	return true;
}

void tipc_dest_list_purge(struct list_head *l)
{
	struct tipc_dest *dst, *tmp;

	list_for_each_entry_safe(dst, tmp, l, list) {
		list_del(&dst->list);
		kfree(dst);
	}
}

int tipc_dest_list_len(struct list_head *l)
{
	struct tipc_dest *dst;
	int i = 0;

	list_for_each_entry(dst, l, list) {
		i++;
	}
	return i;
}
