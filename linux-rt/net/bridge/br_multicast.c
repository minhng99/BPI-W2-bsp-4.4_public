/*
 * Bridge multicast support.
 *
 * Copyright (c) 2010 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <linux/err.h>
#include <linux/export.h>
#include <linux/if_ether.h>
#include <linux/igmp.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/netdevice.h>
#include <linux/netfilter_bridge.h>
#include <linux/random.h>
#include <linux/rculist.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/inetdevice.h>
#include <net/ip.h>
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
#include <net/mld.h>
#include <net/ip6_checksum.h>
#include <net/addrconf.h>
#endif

#include "br_private.h"
#if defined(CONFIG_RTL_819X)
#include <linux/inetdevice.h>
#include <net/rtl/rtl_nic.h>
#endif /* CONFIG_RTL_819X */
#if defined(CONFIG_RTL_HARDWARE_MULTICAST)
#include <net/rtl/rtl865x_multicast.h>
#include <net/rtl/rtl865x_igmpsnooping.h>
#include "../../drivers/soc/realtek/rtd129x/hw_nat/common/rtl865x_eventMgr.h"
#include "../../drivers/soc/realtek/rtd129x/hw_nat/AsicDriver/rtl865x_asicL3.h"
int32 rtl865x_updateHwMulticast(struct net_bridge *br,
				__be32 group);
#if defined(MCAST_TO_UNICAST)
#if defined(IPV6_MCAST_TO_UNICAST)
static char igmp_type_check(struct sk_buff *skb, unsigned char *gmac,unsigned int *gIndex,unsigned int *moreFlag);
static void br_update_igmp_snoop_fdb(unsigned char op,
				     struct net_bridge *br,
				     struct net_bridge_port *p,
				     unsigned char *gmac,
				     struct sk_buff *skb);
static char ICMPv6_check(struct sk_buff *skb , unsigned char *gmac);
#endif /* IPV6_MCAST_TO_UNICAST */
#endif /* MCAST_TO_UNICAST */
#endif /* CONFIG_RTL_HARDWARE_MULTICAST */

static void br_multicast_start_querier(struct net_bridge *br,
				       struct bridge_mcast_own_query *query);
static void br_multicast_add_router(struct net_bridge *br,
				    struct net_bridge_port *port);
static void br_ip4_multicast_leave_group(struct net_bridge *br,
					 struct net_bridge_port *port,
					 __be32 group,
					 __u16 vid);
#if IS_ENABLED(CONFIG_IPV6)
static void br_ip6_multicast_leave_group(struct net_bridge *br,
					 struct net_bridge_port *port,
					 const struct in6_addr *group,
					 __u16 vid);
#endif
unsigned int br_mdb_rehash_seq;

static inline int br_ip_equal(const struct br_ip *a, const struct br_ip *b)
{
	if (a->proto != b->proto)
		return 0;
	if (a->vid != b->vid)
		return 0;
	switch (a->proto) {
	case htons(ETH_P_IP):
		return a->u.ip4 == b->u.ip4;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		return ipv6_addr_equal(&a->u.ip6, &b->u.ip6);
#endif
	}
	return 0;
}

static inline int __br_ip4_hash(struct net_bridge_mdb_htable *mdb, __be32 ip,
				__u16 vid)
{
	return jhash_2words((__force u32)ip, vid, mdb->secret) & (mdb->max - 1);
}

#if IS_ENABLED(CONFIG_IPV6)
static inline int __br_ip6_hash(struct net_bridge_mdb_htable *mdb,
				const struct in6_addr *ip,
				__u16 vid)
{
	return jhash_2words(ipv6_addr_hash(ip), vid,
			    mdb->secret) & (mdb->max - 1);
}
#endif

static inline int br_ip_hash(struct net_bridge_mdb_htable *mdb,
			     struct br_ip *ip)
{
	switch (ip->proto) {
	case htons(ETH_P_IP):
		return __br_ip4_hash(mdb, ip->u.ip4, ip->vid);
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		return __br_ip6_hash(mdb, &ip->u.ip6, ip->vid);
#endif
	}
	return 0;
}

static struct net_bridge_mdb_entry *__br_mdb_ip_get(
	struct net_bridge_mdb_htable *mdb, struct br_ip *dst, int hash)
{
	struct net_bridge_mdb_entry *mp;

	hlist_for_each_entry_rcu(mp, &mdb->mhash[hash], hlist[mdb->ver]) {
		if (br_ip_equal(&mp->addr, dst))
			return mp;
	}

	return NULL;
}

struct net_bridge_mdb_entry *br_mdb_ip_get(struct net_bridge_mdb_htable *mdb,
					   struct br_ip *dst)
{
	if (!mdb)
		return NULL;

	return __br_mdb_ip_get(mdb, dst, br_ip_hash(mdb, dst));
}

static struct net_bridge_mdb_entry *br_mdb_ip4_get(
	struct net_bridge_mdb_htable *mdb, __be32 dst, __u16 vid)
{
	struct br_ip br_dst;

	br_dst.u.ip4 = dst;
	br_dst.proto = htons(ETH_P_IP);
	br_dst.vid = vid;

	return br_mdb_ip_get(mdb, &br_dst);
}

#if IS_ENABLED(CONFIG_IPV6)
static struct net_bridge_mdb_entry *br_mdb_ip6_get(
	struct net_bridge_mdb_htable *mdb, const struct in6_addr *dst,
	__u16 vid)
{
	struct br_ip br_dst;

	br_dst.u.ip6 = *dst;
	br_dst.proto = htons(ETH_P_IPV6);
	br_dst.vid = vid;

	return br_mdb_ip_get(mdb, &br_dst);
}
#endif

struct net_bridge_mdb_entry *br_mdb_get(struct net_bridge *br,
					struct sk_buff *skb, u16 vid)
{
	struct net_bridge_mdb_htable *mdb = rcu_dereference(br->mdb);
	struct br_ip ip;

	if (br->multicast_disabled)
		return NULL;

	if (BR_INPUT_SKB_CB(skb)->igmp)
		return NULL;

	ip.proto = skb->protocol;
	ip.vid = vid;

	switch (skb->protocol) {
	case htons(ETH_P_IP):
		ip.u.ip4 = ip_hdr(skb)->daddr;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		ip.u.ip6 = ipv6_hdr(skb)->daddr;
		break;
#endif
	default:
		return NULL;
	}

	return br_mdb_ip_get(mdb, &ip);
}

static void br_mdb_free(struct rcu_head *head)
{
	struct net_bridge_mdb_htable *mdb =
		container_of(head, struct net_bridge_mdb_htable, rcu);
	struct net_bridge_mdb_htable *old = mdb->old;

	mdb->old = NULL;
	kfree(old->mhash);
	kfree(old);
}

static int br_mdb_copy(struct net_bridge_mdb_htable *new,
		       struct net_bridge_mdb_htable *old,
		       int elasticity)
{
	struct net_bridge_mdb_entry *mp;
	int maxlen;
	int len;
	int i;

	for (i = 0; i < old->max; i++)
		hlist_for_each_entry(mp, &old->mhash[i], hlist[old->ver])
			hlist_add_head(&mp->hlist[new->ver],
				       &new->mhash[br_ip_hash(new, &mp->addr)]);

	if (!elasticity)
		return 0;

	maxlen = 0;
	for (i = 0; i < new->max; i++) {
		len = 0;
		hlist_for_each_entry(mp, &new->mhash[i], hlist[new->ver])
			len++;
		if (len > maxlen)
			maxlen = len;
	}

	return maxlen > elasticity ? -EINVAL : 0;
}

void br_multicast_free_pg(struct rcu_head *head)
{
	struct net_bridge_port_group *p =
		container_of(head, struct net_bridge_port_group, rcu);

	kfree(p);
}

static void br_multicast_free_group(struct rcu_head *head)
{
	struct net_bridge_mdb_entry *mp =
		container_of(head, struct net_bridge_mdb_entry, rcu);

	kfree(mp);
}

static void br_multicast_group_expired(unsigned long data)
{
	struct net_bridge_mdb_entry *mp = (void *)data;
	struct net_bridge *br = mp->br;
	struct net_bridge_mdb_htable *mdb;

	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) || timer_pending(&mp->timer))
		goto out;

	mp->mglist = false;

	if (mp->ports)
		goto out;

	mdb = mlock_dereference(br->mdb, br);

	hlist_del_rcu(&mp->hlist[mdb->ver]);
	mdb->size--;

	call_rcu_bh(&mp->rcu, br_multicast_free_group);

out:
#if defined(CONFIG_RTL_HARDWARE_MULTICAST)
	rtl865x_updateHwMulticast(br, mp->addr.u.ip4);
#endif /* CONFIG_RTL_HARDWARE_MULTICAST */
	spin_unlock(&br->multicast_lock);
}

static void br_multicast_del_pg(struct net_bridge *br,
				struct net_bridge_port_group *pg)
{
	struct net_bridge_mdb_htable *mdb;
	struct net_bridge_mdb_entry *mp;
	struct net_bridge_port_group *p;
	struct net_bridge_port_group __rcu **pp;

	mdb = mlock_dereference(br->mdb, br);

	mp = br_mdb_ip_get(mdb, &pg->addr);
	if (WARN_ON(!mp))
		return;

	for (pp = &mp->ports;
	     (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (p != pg)
			continue;

		rcu_assign_pointer(*pp, p->next);
		hlist_del_init(&p->mglist);
		del_timer(&p->timer);
		br_mdb_notify(br->dev, p->port, &pg->addr, RTM_DELMDB,
			      p->state);
		call_rcu_bh(&p->rcu, br_multicast_free_pg);

		if (!mp->ports && !mp->mglist &&
		    netif_running(br->dev))
			mod_timer(&mp->timer, jiffies);

		return;
	}

	WARN_ON(1);
}

static void br_multicast_port_group_expired(unsigned long data)
{
	struct net_bridge_port_group *pg = (void *)data;
	struct net_bridge *br = pg->port->br;

	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) || timer_pending(&pg->timer) ||
	    hlist_unhashed(&pg->mglist) || pg->state & MDB_PERMANENT)
		goto out;

	br_multicast_del_pg(br, pg);

out:
#if defined(CONFIG_RTL_HARDWARE_MULTICAST)
	rtl865x_updateHwMulticast(br, pg->addr.u.ip4);
#endif /* CONFIG_RTL_HARDWARE_MULTICAST */
	spin_unlock(&br->multicast_lock);
}

static int br_mdb_rehash(struct net_bridge_mdb_htable __rcu **mdbp, int max,
			 int elasticity)
{
	struct net_bridge_mdb_htable *old = rcu_dereference_protected(*mdbp, 1);
	struct net_bridge_mdb_htable *mdb;
	int err;

	mdb = kmalloc(sizeof(*mdb), GFP_ATOMIC);
	if (!mdb)
		return -ENOMEM;

	mdb->max = max;
	mdb->old = old;

	mdb->mhash = kzalloc(max * sizeof(*mdb->mhash), GFP_ATOMIC);
	if (!mdb->mhash) {
		kfree(mdb);
		return -ENOMEM;
	}

	mdb->size = old ? old->size : 0;
	mdb->ver = old ? old->ver ^ 1 : 0;

	if (!old || elasticity)
		get_random_bytes(&mdb->secret, sizeof(mdb->secret));
	else
		mdb->secret = old->secret;

	if (!old)
		goto out;

	err = br_mdb_copy(mdb, old, elasticity);
	if (err) {
		kfree(mdb->mhash);
		kfree(mdb);
		return err;
	}

	br_mdb_rehash_seq++;
	call_rcu_bh(&mdb->rcu, br_mdb_free);

out:
	rcu_assign_pointer(*mdbp, mdb);

	return 0;
}
#if defined(CONFIG_RTL_819X)
static __be32 get_br_ip(void)
{
	struct in_device *in_dev;
	struct net_device *landev;
	struct in_ifaddr *ifap = NULL;
	if ((landev = __dev_get_by_name(&init_net, RTL_PS_BR0_DEV_NAME)) != NULL)
	{
		#if defined(CONFIG_RTD_1295_HWNAT)
		in_dev = landev->ip_ptr;
		#else /* CONFIG_RTD_1295_HWNAT */
		in_dev=(struct net_device *)(landev->ip_ptr);
		#endif /* CONFIG_RTD_1295_HWNAT */
		if (in_dev != NULL)
		{
			for (ifap=in_dev->ifa_list; ifap != NULL; ifap=ifap->ifa_next)
			{
				if (strcmp(RTL_PS_BR0_DEV_NAME, ifap->ifa_label) == 0)
				{
					return ifap->ifa_address;
				}
			}

		}
	}
	return 0;
}
#endif /* CONFIG_RTL_819X */

static struct sk_buff *br_ip4_multicast_alloc_query(struct net_bridge *br,
						    __be32 group)
{
	struct sk_buff *skb;
	struct igmphdr *ih;
	struct ethhdr *eth;
	struct iphdr *iph;

	skb = netdev_alloc_skb_ip_align(br->dev, sizeof(*eth) + sizeof(*iph) +
						 sizeof(*ih) + 4);
	if (!skb)
		goto out;

	skb->protocol = htons(ETH_P_IP);

	skb_reset_mac_header(skb);
	eth = eth_hdr(skb);

	ether_addr_copy(eth->h_source, br->dev->dev_addr);
	eth->h_dest[0] = 1;
	eth->h_dest[1] = 0;
	eth->h_dest[2] = 0x5e;
	eth->h_dest[3] = 0;
	eth->h_dest[4] = 0;
	eth->h_dest[5] = 1;
	eth->h_proto = htons(ETH_P_IP);
	skb_put(skb, sizeof(*eth));

	skb_set_network_header(skb, skb->len);
	iph = ip_hdr(skb);

	iph->version = 4;
	iph->ihl = 6;
	iph->tos = 0xc0;
	iph->tot_len = htons(sizeof(*iph) + sizeof(*ih) + 4);
	iph->id = 0;
	iph->frag_off = htons(IP_DF);
	iph->ttl = 1;
	iph->protocol = IPPROTO_IGMP;
#if defined(CONFIG_RTL_819X)
	iph->saddr = get_br_ip();
#else
	iph->saddr = br->multicast_query_use_ifaddr ?
		     inet_select_addr(br->dev, 0, RT_SCOPE_LINK) : 0;
#endif /* CONFIG_RTL_819X */
	iph->daddr = htonl(INADDR_ALLHOSTS_GROUP);
	((u8 *)&iph[1])[0] = IPOPT_RA;
	((u8 *)&iph[1])[1] = 4;
	((u8 *)&iph[1])[2] = 0;
	((u8 *)&iph[1])[3] = 0;
	ip_send_check(iph);
	skb_put(skb, 24);

	skb_set_transport_header(skb, skb->len);
	ih = igmp_hdr(skb);
	ih->type = IGMP_HOST_MEMBERSHIP_QUERY;
	ih->code = (group ? br->multicast_last_member_interval :
			    br->multicast_query_response_interval) /
		   (HZ / IGMP_TIMER_SCALE);
	ih->group = group;
	ih->csum = 0;
	ih->csum = ip_compute_csum((void *)ih, sizeof(struct igmphdr));
	skb_put(skb, sizeof(*ih));

	__skb_pull(skb, sizeof(*eth));

out:
	return skb;
}

#if IS_ENABLED(CONFIG_IPV6)
static struct sk_buff *br_ip6_multicast_alloc_query(struct net_bridge *br,
						    const struct in6_addr *group)
{
	struct sk_buff *skb;
	struct ipv6hdr *ip6h;
	struct mld_msg *mldq;
	struct ethhdr *eth;
	u8 *hopopt;
	unsigned long interval;

	skb = netdev_alloc_skb_ip_align(br->dev, sizeof(*eth) + sizeof(*ip6h) +
						 8 + sizeof(*mldq));
	if (!skb)
		goto out;

	skb->protocol = htons(ETH_P_IPV6);

	/* Ethernet header */
	skb_reset_mac_header(skb);
	eth = eth_hdr(skb);

	ether_addr_copy(eth->h_source, br->dev->dev_addr);
	eth->h_proto = htons(ETH_P_IPV6);
	skb_put(skb, sizeof(*eth));

	/* IPv6 header + HbH option */
	skb_set_network_header(skb, skb->len);
	ip6h = ipv6_hdr(skb);

	*(__force __be32 *)ip6h = htonl(0x60000000);
	ip6h->payload_len = htons(8 + sizeof(*mldq));
	ip6h->nexthdr = IPPROTO_HOPOPTS;
	ip6h->hop_limit = 1;
	ipv6_addr_set(&ip6h->daddr, htonl(0xff020000), 0, 0, htonl(1));
	if (ipv6_dev_get_saddr(dev_net(br->dev), br->dev, &ip6h->daddr, 0,
			       &ip6h->saddr)) {
		kfree_skb(skb);
		br->has_ipv6_addr = 0;
		return NULL;
	}

	br->has_ipv6_addr = 1;
	ipv6_eth_mc_map(&ip6h->daddr, eth->h_dest);

	hopopt = (u8 *)(ip6h + 1);
	hopopt[0] = IPPROTO_ICMPV6;		/* next hdr */
	hopopt[1] = 0;				/* length of HbH */
	hopopt[2] = IPV6_TLV_ROUTERALERT;	/* Router Alert */
	hopopt[3] = 2;				/* Length of RA Option */
	hopopt[4] = 0;				/* Type = 0x0000 (MLD) */
	hopopt[5] = 0;
	hopopt[6] = IPV6_TLV_PAD1;		/* Pad1 */
	hopopt[7] = IPV6_TLV_PAD1;		/* Pad1 */

	skb_put(skb, sizeof(*ip6h) + 8);

	/* ICMPv6 */
	skb_set_transport_header(skb, skb->len);
	mldq = (struct mld_msg *) icmp6_hdr(skb);

	interval = ipv6_addr_any(group) ?
			br->multicast_query_response_interval :
			br->multicast_last_member_interval;

	mldq->mld_type = ICMPV6_MGM_QUERY;
	mldq->mld_code = 0;
	mldq->mld_cksum = 0;
	mldq->mld_maxdelay = htons((u16)jiffies_to_msecs(interval));
	mldq->mld_reserved = 0;
	mldq->mld_mca = *group;

	/* checksum */
	mldq->mld_cksum = csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr,
					  sizeof(*mldq), IPPROTO_ICMPV6,
					  csum_partial(mldq,
						       sizeof(*mldq), 0));
	skb_put(skb, sizeof(*mldq));

	__skb_pull(skb, sizeof(*eth));

out:
	return skb;
}
#endif

static struct sk_buff *br_multicast_alloc_query(struct net_bridge *br,
						struct br_ip *addr)
{
	switch (addr->proto) {
	case htons(ETH_P_IP):
		return br_ip4_multicast_alloc_query(br, addr->u.ip4);
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		return br_ip6_multicast_alloc_query(br, &addr->u.ip6);
#endif
	}
	return NULL;
}

static struct net_bridge_mdb_entry *br_multicast_get_group(
	struct net_bridge *br, struct net_bridge_port *port,
	struct br_ip *group, int hash)
{
	struct net_bridge_mdb_htable *mdb;
	struct net_bridge_mdb_entry *mp;
	unsigned int count = 0;
	unsigned int max;
	int elasticity;
	int err;

	mdb = rcu_dereference_protected(br->mdb, 1);
	hlist_for_each_entry(mp, &mdb->mhash[hash], hlist[mdb->ver]) {
		count++;
		if (unlikely(br_ip_equal(group, &mp->addr)))
			return mp;
	}

	elasticity = 0;
	max = mdb->max;

	if (unlikely(count > br->hash_elasticity && count)) {
		if (net_ratelimit())
			br_info(br, "Multicast hash table "
				"chain limit reached: %s\n",
				port ? port->dev->name : br->dev->name);

		elasticity = br->hash_elasticity;
	}

	if (mdb->size >= max) {
		max *= 2;
		if (unlikely(max > br->hash_max)) {
			br_warn(br, "Multicast hash table maximum of %d "
				"reached, disabling snooping: %s\n",
				br->hash_max,
				port ? port->dev->name : br->dev->name);
			err = -E2BIG;
disable:
			br->multicast_disabled = 1;
			goto err;
		}
	}

	if (max > mdb->max || elasticity) {
		if (mdb->old) {
			if (net_ratelimit())
				br_info(br, "Multicast hash table "
					"on fire: %s\n",
					port ? port->dev->name : br->dev->name);
			err = -EEXIST;
			goto err;
		}

		err = br_mdb_rehash(&br->mdb, max, elasticity);
		if (err) {
			br_warn(br, "Cannot rehash multicast "
				"hash table, disabling snooping: %s, %d, %d\n",
				port ? port->dev->name : br->dev->name,
				mdb->size, err);
			goto disable;
		}

		err = -EAGAIN;
		goto err;
	}

	return NULL;

err:
	mp = ERR_PTR(err);
	return mp;
}

struct net_bridge_mdb_entry *br_multicast_new_group(struct net_bridge *br,
	struct net_bridge_port *port, struct br_ip *group)
{
	struct net_bridge_mdb_htable *mdb;
	struct net_bridge_mdb_entry *mp;
	int hash;
	int err;

	mdb = rcu_dereference_protected(br->mdb, 1);
	if (!mdb) {
		err = br_mdb_rehash(&br->mdb, BR_HASH_SIZE, 0);
		if (err)
			return ERR_PTR(err);
		goto rehash;
	}

	hash = br_ip_hash(mdb, group);
	mp = br_multicast_get_group(br, port, group, hash);
	switch (PTR_ERR(mp)) {
	case 0:
		break;

	case -EAGAIN:
rehash:
		mdb = rcu_dereference_protected(br->mdb, 1);
		hash = br_ip_hash(mdb, group);
		break;

	default:
		goto out;
	}

	mp = kzalloc(sizeof(*mp), GFP_ATOMIC);
	if (unlikely(!mp))
		return ERR_PTR(-ENOMEM);

	mp->br = br;
	mp->addr = *group;
	setup_timer(&mp->timer, br_multicast_group_expired,
		    (unsigned long)mp);

	hlist_add_head_rcu(&mp->hlist[mdb->ver], &mdb->mhash[hash]);
	mdb->size++;

out:
	return mp;
}

struct net_bridge_port_group *br_multicast_new_port_group(
			struct net_bridge_port *port,
			struct br_ip *group,
			struct net_bridge_port_group __rcu *next,
			unsigned char state)
{
	struct net_bridge_port_group *p;

	p = kzalloc(sizeof(*p), GFP_ATOMIC);
	if (unlikely(!p))
		return NULL;

	p->addr = *group;
	p->port = port;
	p->state = state;
	rcu_assign_pointer(p->next, next);
	hlist_add_head(&p->mglist, &port->mglist);
	setup_timer(&p->timer, br_multicast_port_group_expired,
		    (unsigned long)p);
	return p;
}

static int br_multicast_add_group(struct net_bridge *br,
				  struct net_bridge_port *port,
				  struct br_ip *group)
{
	struct net_bridge_mdb_entry *mp;
	struct net_bridge_port_group *p;
	struct net_bridge_port_group __rcu **pp;
	unsigned long now = jiffies;
	int err;

	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) ||
	    (port && port->state == BR_STATE_DISABLED))
		goto out;

	mp = br_multicast_new_group(br, port, group);
	err = PTR_ERR(mp);
	if (IS_ERR(mp))
		goto err;

	if (!port) {
		mp->mglist = true;
		mod_timer(&mp->timer, now + br->multicast_membership_interval);
		goto out;
	}

	for (pp = &mp->ports;
	     (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (p->port == port)
			goto found;
		if ((unsigned long)p->port < (unsigned long)port)
			break;
	}

	p = br_multicast_new_port_group(port, group, *pp, MDB_TEMPORARY);
	if (unlikely(!p))
		goto err;
	rcu_assign_pointer(*pp, p);
	br_mdb_notify(br->dev, port, group, RTM_NEWMDB, MDB_TEMPORARY);

found:
	mod_timer(&p->timer, now + br->multicast_membership_interval);
out:
	err = 0;

err:
	spin_unlock(&br->multicast_lock);
	return err;
}

static int br_ip4_multicast_add_group(struct net_bridge *br,
				      struct net_bridge_port *port,
				      __be32 group,
				      __u16 vid)
{
	struct br_ip br_group;

	if (ipv4_is_local_multicast(group))
		return 0;

	br_group.u.ip4 = group;
	br_group.proto = htons(ETH_P_IP);
	br_group.vid = vid;

	return br_multicast_add_group(br, port, &br_group);
}

#if IS_ENABLED(CONFIG_IPV6)
static int br_ip6_multicast_add_group(struct net_bridge *br,
				      struct net_bridge_port *port,
				      const struct in6_addr *group,
				      __u16 vid)
{
	struct br_ip br_group;

	if (ipv6_addr_is_ll_all_nodes(group))
		return 0;

	br_group.u.ip6 = *group;
	br_group.proto = htons(ETH_P_IPV6);
	br_group.vid = vid;

	return br_multicast_add_group(br, port, &br_group);
}
#endif

static void br_multicast_router_expired(unsigned long data)
{
	struct net_bridge_port *port = (void *)data;
	struct net_bridge *br = port->br;

	spin_lock(&br->multicast_lock);
	if (port->multicast_router != 1 ||
	    timer_pending(&port->multicast_router_timer) ||
	    hlist_unhashed(&port->rlist))
		goto out;

	hlist_del_init_rcu(&port->rlist);
	br_rtr_notify(br->dev, port, RTM_DELMDB);

out:
	spin_unlock(&br->multicast_lock);
}

static void br_multicast_local_router_expired(unsigned long data)
{
}

static void br_multicast_querier_expired(struct net_bridge *br,
					 struct bridge_mcast_own_query *query)
{
	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) || br->multicast_disabled)
		goto out;

	br_multicast_start_querier(br, query);

out:
	spin_unlock(&br->multicast_lock);
}

static void br_ip4_multicast_querier_expired(unsigned long data)
{
	struct net_bridge *br = (void *)data;

	br_multicast_querier_expired(br, &br->ip4_own_query);
}

#if IS_ENABLED(CONFIG_IPV6)
static void br_ip6_multicast_querier_expired(unsigned long data)
{
	struct net_bridge *br = (void *)data;

	br_multicast_querier_expired(br, &br->ip6_own_query);
}
#endif

static void br_multicast_select_own_querier(struct net_bridge *br,
					    struct br_ip *ip,
					    struct sk_buff *skb)
{
	if (ip->proto == htons(ETH_P_IP))
		br->ip4_querier.addr.u.ip4 = ip_hdr(skb)->saddr;
#if IS_ENABLED(CONFIG_IPV6)
	else
		br->ip6_querier.addr.u.ip6 = ipv6_hdr(skb)->saddr;
#endif
}

static void __br_multicast_send_query(struct net_bridge *br,
				      struct net_bridge_port *port,
				      struct br_ip *ip)
{
	struct sk_buff *skb;

	skb = br_multicast_alloc_query(br, ip);
	if (!skb)
		return;

	if (port) {
		skb->dev = port->dev;
		NF_HOOK(NFPROTO_BRIDGE, NF_BR_LOCAL_OUT,
			dev_net(port->dev), NULL, skb, NULL, skb->dev,
			br_dev_queue_push_xmit);
	} else {
		br_multicast_select_own_querier(br, ip, skb);
		netif_rx(skb);
	}
}

static void br_multicast_send_query(struct net_bridge *br,
				    struct net_bridge_port *port,
				    struct bridge_mcast_own_query *own_query)
{
	unsigned long time;
	struct br_ip br_group;
	struct bridge_mcast_other_query *other_query = NULL;

	if (!netif_running(br->dev) || br->multicast_disabled ||
	    !br->multicast_querier)
		return;

	memset(&br_group.u, 0, sizeof(br_group.u));

	if (port ? (own_query == &port->ip4_own_query) :
		   (own_query == &br->ip4_own_query)) {
		other_query = &br->ip4_other_query;
		br_group.proto = htons(ETH_P_IP);
#if IS_ENABLED(CONFIG_IPV6)
	} else {
		other_query = &br->ip6_other_query;
		br_group.proto = htons(ETH_P_IPV6);
#endif
	}

	if (!other_query || timer_pending(&other_query->timer))
		return;

	__br_multicast_send_query(br, port, &br_group);

	time = jiffies;
	time += own_query->startup_sent < br->multicast_startup_query_count ?
		br->multicast_startup_query_interval :
		br->multicast_query_interval;
	mod_timer(&own_query->timer, time);
}

static void
br_multicast_port_query_expired(struct net_bridge_port *port,
				struct bridge_mcast_own_query *query)
{
	struct net_bridge *br = port->br;

	spin_lock(&br->multicast_lock);
	if (port->state == BR_STATE_DISABLED ||
	    port->state == BR_STATE_BLOCKING)
		goto out;

	if (query->startup_sent < br->multicast_startup_query_count)
		query->startup_sent++;

	br_multicast_send_query(port->br, port, query);

out:
	spin_unlock(&br->multicast_lock);
}

static void br_ip4_multicast_port_query_expired(unsigned long data)
{
	struct net_bridge_port *port = (void *)data;

	br_multicast_port_query_expired(port, &port->ip4_own_query);
}

#if IS_ENABLED(CONFIG_IPV6)
static void br_ip6_multicast_port_query_expired(unsigned long data)
{
	struct net_bridge_port *port = (void *)data;

	br_multicast_port_query_expired(port, &port->ip6_own_query);
}
#endif

void br_multicast_add_port(struct net_bridge_port *port)
{
	port->multicast_router = 1;

	setup_timer(&port->multicast_router_timer, br_multicast_router_expired,
		    (unsigned long)port);
	setup_timer(&port->ip4_own_query.timer,
		    br_ip4_multicast_port_query_expired, (unsigned long)port);
#if IS_ENABLED(CONFIG_IPV6)
	setup_timer(&port->ip6_own_query.timer,
		    br_ip6_multicast_port_query_expired, (unsigned long)port);
#endif
}

void br_multicast_del_port(struct net_bridge_port *port)
{
	struct net_bridge *br = port->br;
	struct net_bridge_port_group *pg;
	struct hlist_node *n;

	/* Take care of the remaining groups, only perm ones should be left */
	spin_lock_bh(&br->multicast_lock);
	hlist_for_each_entry_safe(pg, n, &port->mglist, mglist)
		br_multicast_del_pg(br, pg);
	spin_unlock_bh(&br->multicast_lock);
	del_timer_sync(&port->multicast_router_timer);
}

static void br_multicast_enable(struct bridge_mcast_own_query *query)
{
	query->startup_sent = 0;

	if (try_to_del_timer_sync(&query->timer) >= 0 ||
	    del_timer(&query->timer))
		mod_timer(&query->timer, jiffies);
}

void br_multicast_enable_port(struct net_bridge_port *port)
{
	struct net_bridge *br = port->br;

	spin_lock(&br->multicast_lock);
	if (br->multicast_disabled || !netif_running(br->dev))
		goto out;

	br_multicast_enable(&port->ip4_own_query);
#if IS_ENABLED(CONFIG_IPV6)
	br_multicast_enable(&port->ip6_own_query);
#endif
	if (port->multicast_router == 2 && hlist_unhashed(&port->rlist))
		br_multicast_add_router(br, port);

out:
	spin_unlock(&br->multicast_lock);
}

void br_multicast_disable_port(struct net_bridge_port *port)
{
	struct net_bridge *br = port->br;
	struct net_bridge_port_group *pg;
	struct hlist_node *n;

	spin_lock(&br->multicast_lock);
	hlist_for_each_entry_safe(pg, n, &port->mglist, mglist)
		if (pg->state == MDB_TEMPORARY)
			br_multicast_del_pg(br, pg);

	if (!hlist_unhashed(&port->rlist)) {
		hlist_del_init_rcu(&port->rlist);
		br_rtr_notify(br->dev, port, RTM_DELMDB);
	}
	del_timer(&port->multicast_router_timer);
	del_timer(&port->ip4_own_query.timer);
#if IS_ENABLED(CONFIG_IPV6)
	del_timer(&port->ip6_own_query.timer);
#endif
	spin_unlock(&br->multicast_lock);
}

static int br_ip4_multicast_igmp3_report(struct net_bridge *br,
					 struct net_bridge_port *port,
					 struct sk_buff *skb,
					 u16 vid)
{
#if defined(CONFIG_RTL_819X)
	struct iphdr *iph;
#endif /* CONFIG_RTL_819X */
	struct igmpv3_report *ih;
	struct igmpv3_grec *grec;
	int i;
	int len;
	int num;
	int type;
	int err = 0;
	__be32 group;

#if defined(CONFIG_RTL_819X)
	iph=ip_hdr(skb);
#endif /* CONFIG_RTL_819X */
	ih = igmpv3_report_hdr(skb);
	num = ntohs(ih->ngrec);
	len = skb_transport_offset(skb) + sizeof(*ih);

	for (i = 0; i < num; i++) {
		len += sizeof(*grec);
		if (!pskb_may_pull(skb, len))
			return -EINVAL;

		grec = (void *)(skb->data + len - sizeof(*grec));
		group = grec->grec_mca;
		type = grec->grec_type;

		len += ntohs(grec->grec_nsrcs) * 4;
		if (!pskb_may_pull(skb, len))
			return -EINVAL;

		/* We treat this as an IGMPv2 report for now. */
		switch (type) {
		case IGMPV3_MODE_IS_INCLUDE:
		case IGMPV3_MODE_IS_EXCLUDE:
		case IGMPV3_CHANGE_TO_INCLUDE:
		case IGMPV3_CHANGE_TO_EXCLUDE:
		case IGMPV3_ALLOW_NEW_SOURCES:
		case IGMPV3_BLOCK_OLD_SOURCES:
			break;

		default:
			continue;
		}

		if ((type == IGMPV3_CHANGE_TO_INCLUDE ||
		     type == IGMPV3_MODE_IS_INCLUDE) &&
		    ntohs(grec->grec_nsrcs) == 0) {
			br_ip4_multicast_leave_group(br, port, group, vid);
		} else {
			err = br_ip4_multicast_add_group(br, port, group, vid);
			if (err)
				break;
#if defined(CONFIG_RTL_HARDWARE_MULTICAST)
			if ((type != IGMPV3_ALLOW_NEW_SOURCES) &&
			    (type != IGMPV3_BLOCK_OLD_SOURCES))
				rtl865x_updateHwMulticast(br, group);
#endif /* CONFIG_RTL_HARDWARE_MULTICAST */
		}
	}

	return err;
}

#if IS_ENABLED(CONFIG_IPV6)
static int br_ip6_multicast_mld2_report(struct net_bridge *br,
					struct net_bridge_port *port,
					struct sk_buff *skb,
					u16 vid)
{
	struct icmp6hdr *icmp6h;
	struct mld2_grec *grec;
	int i;
	int len;
	int num;
	int err = 0;

	if (!pskb_may_pull(skb, sizeof(*icmp6h)))
		return -EINVAL;

	icmp6h = icmp6_hdr(skb);
	num = ntohs(icmp6h->icmp6_dataun.un_data16[1]);
	len = skb_transport_offset(skb) + sizeof(*icmp6h);

	for (i = 0; i < num; i++) {
		__be16 *nsrcs, _nsrcs;

		nsrcs = skb_header_pointer(skb,
					   len + offsetof(struct mld2_grec,
							  grec_nsrcs),
					   sizeof(_nsrcs), &_nsrcs);
		if (!nsrcs)
			return -EINVAL;

		if (!pskb_may_pull(skb,
				   len + sizeof(*grec) +
				   sizeof(struct in6_addr) * ntohs(*nsrcs)))
			return -EINVAL;

		grec = (struct mld2_grec *)(skb->data + len);
		len += sizeof(*grec) +
		       sizeof(struct in6_addr) * ntohs(*nsrcs);

		/* We treat these as MLDv1 reports for now. */
		switch (grec->grec_type) {
		case MLD2_MODE_IS_INCLUDE:
		case MLD2_MODE_IS_EXCLUDE:
		case MLD2_CHANGE_TO_INCLUDE:
		case MLD2_CHANGE_TO_EXCLUDE:
		case MLD2_ALLOW_NEW_SOURCES:
		case MLD2_BLOCK_OLD_SOURCES:
			break;

		default:
			continue;
		}

		if ((grec->grec_type == MLD2_CHANGE_TO_INCLUDE ||
		     grec->grec_type == MLD2_MODE_IS_INCLUDE) &&
		    ntohs(*nsrcs) == 0) {
			br_ip6_multicast_leave_group(br, port, &grec->grec_mca,
						     vid);
		} else {
			err = br_ip6_multicast_add_group(br, port,
							 &grec->grec_mca, vid);
			if (!err)
				break;
		}
	}

	return err;
}
#endif

static bool br_ip4_multicast_select_querier(struct net_bridge *br,
					    struct net_bridge_port *port,
					    __be32 saddr)
{
	if (!timer_pending(&br->ip4_own_query.timer) &&
	    !timer_pending(&br->ip4_other_query.timer))
		goto update;

	if (!br->ip4_querier.addr.u.ip4)
		goto update;

	if (ntohl(saddr) <= ntohl(br->ip4_querier.addr.u.ip4))
		goto update;

	return false;

update:
	br->ip4_querier.addr.u.ip4 = saddr;

	/* update protected by general multicast_lock by caller */
	rcu_assign_pointer(br->ip4_querier.port, port);

	return true;
}

#if IS_ENABLED(CONFIG_IPV6)
static bool br_ip6_multicast_select_querier(struct net_bridge *br,
					    struct net_bridge_port *port,
					    struct in6_addr *saddr)
{
	if (!timer_pending(&br->ip6_own_query.timer) &&
	    !timer_pending(&br->ip6_other_query.timer))
		goto update;

	if (ipv6_addr_cmp(saddr, &br->ip6_querier.addr.u.ip6) <= 0)
		goto update;

	return false;

update:
	br->ip6_querier.addr.u.ip6 = *saddr;

	/* update protected by general multicast_lock by caller */
	rcu_assign_pointer(br->ip6_querier.port, port);

	return true;
}
#endif

static bool br_multicast_select_querier(struct net_bridge *br,
					struct net_bridge_port *port,
					struct br_ip *saddr)
{
	switch (saddr->proto) {
	case htons(ETH_P_IP):
		return br_ip4_multicast_select_querier(br, port, saddr->u.ip4);
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		return br_ip6_multicast_select_querier(br, port, &saddr->u.ip6);
#endif
	}

	return false;
}

static void
br_multicast_update_query_timer(struct net_bridge *br,
				struct bridge_mcast_other_query *query,
				unsigned long max_delay)
{
	if (!timer_pending(&query->timer))
		query->delay_time = jiffies + max_delay;

	mod_timer(&query->timer, jiffies + br->multicast_querier_interval);
}

/*
 * Add port to router_list
 *  list is maintained ordered by pointer value
 *  and locked by br->multicast_lock and RCU
 */
static void br_multicast_add_router(struct net_bridge *br,
				    struct net_bridge_port *port)
{
	struct net_bridge_port *p;
	struct hlist_node *slot = NULL;

	if (!hlist_unhashed(&port->rlist))
		return;

	hlist_for_each_entry(p, &br->router_list, rlist) {
		if ((unsigned long) port >= (unsigned long) p)
			break;
		slot = &p->rlist;
	}

	if (slot)
		hlist_add_behind_rcu(&port->rlist, slot);
	else
		hlist_add_head_rcu(&port->rlist, &br->router_list);
	br_rtr_notify(br->dev, port, RTM_NEWMDB);
}

static void br_multicast_mark_router(struct net_bridge *br,
				     struct net_bridge_port *port)
{
	unsigned long now = jiffies;

	if (!port) {
		if (br->multicast_router == 1)
			mod_timer(&br->multicast_router_timer,
				  now + br->multicast_querier_interval);
		return;
	}

	if (port->multicast_router != 1)
		return;

	br_multicast_add_router(br, port);

	mod_timer(&port->multicast_router_timer,
		  now + br->multicast_querier_interval);
}

static void br_multicast_query_received(struct net_bridge *br,
					struct net_bridge_port *port,
					struct bridge_mcast_other_query *query,
					struct br_ip *saddr,
					unsigned long max_delay)
{
	if (!br_multicast_select_querier(br, port, saddr))
		return;

	br_multicast_update_query_timer(br, query, max_delay);
	br_multicast_mark_router(br, port);
}

static int br_ip4_multicast_query(struct net_bridge *br,
				  struct net_bridge_port *port,
				  struct sk_buff *skb,
				  u16 vid)
{
	const struct iphdr *iph = ip_hdr(skb);
	struct igmphdr *ih = igmp_hdr(skb);
	struct net_bridge_mdb_entry *mp;
	struct igmpv3_query *ih3;
	struct net_bridge_port_group *p;
	struct net_bridge_port_group __rcu **pp;
	struct br_ip saddr;
	unsigned long max_delay;
	unsigned long now = jiffies;
	unsigned int offset = skb_transport_offset(skb);
	__be32 group;
	int err = 0;

	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) ||
	    (port && port->state == BR_STATE_DISABLED))
		goto out;

	group = ih->group;

	if (skb->len == offset + sizeof(*ih)) {
		max_delay = ih->code * (HZ / IGMP_TIMER_SCALE);

		if (!max_delay) {
			max_delay = 10 * HZ;
			group = 0;
		}
	} else if (skb->len >= offset + sizeof(*ih3)) {
		ih3 = igmpv3_query_hdr(skb);
		if (ih3->nsrcs)
			goto out;

		max_delay = ih3->code ?
			    IGMPV3_MRC(ih3->code) * (HZ / IGMP_TIMER_SCALE) : 1;
	} else {
		goto out;
	}

	if (!group) {
		saddr.proto = htons(ETH_P_IP);
		saddr.u.ip4 = iph->saddr;

		br_multicast_query_received(br, port, &br->ip4_other_query,
					    &saddr, max_delay);
		goto out;
	}

	mp = br_mdb_ip4_get(mlock_dereference(br->mdb, br), group, vid);
	if (!mp)
		goto out;

	max_delay *= br->multicast_last_member_count;

	if (mp->mglist &&
	    (timer_pending(&mp->timer) ?
	     time_after(mp->timer.expires, now + max_delay) :
	     try_to_del_timer_sync(&mp->timer) >= 0))
		mod_timer(&mp->timer, now + max_delay);

	for (pp = &mp->ports;
	     (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (timer_pending(&p->timer) ?
		    time_after(p->timer.expires, now + max_delay) :
		    try_to_del_timer_sync(&p->timer) >= 0)
			mod_timer(&p->timer, now + max_delay);
	}

out:
	spin_unlock(&br->multicast_lock);
	return err;
}

#if IS_ENABLED(CONFIG_IPV6)
static int br_ip6_multicast_query(struct net_bridge *br,
				  struct net_bridge_port *port,
				  struct sk_buff *skb,
				  u16 vid)
{
	const struct ipv6hdr *ip6h = ipv6_hdr(skb);
	struct mld_msg *mld;
	struct net_bridge_mdb_entry *mp;
	struct mld2_query *mld2q;
	struct net_bridge_port_group *p;
	struct net_bridge_port_group __rcu **pp;
	struct br_ip saddr;
	unsigned long max_delay;
	unsigned long now = jiffies;
	unsigned int offset = skb_transport_offset(skb);
	const struct in6_addr *group = NULL;
	bool is_general_query;
	int err = 0;

	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) ||
	    (port && port->state == BR_STATE_DISABLED))
		goto out;

	if (skb->len == offset + sizeof(*mld)) {
		if (!pskb_may_pull(skb, offset + sizeof(*mld))) {
			err = -EINVAL;
			goto out;
		}
		mld = (struct mld_msg *) icmp6_hdr(skb);
		max_delay = msecs_to_jiffies(ntohs(mld->mld_maxdelay));
		if (max_delay)
			group = &mld->mld_mca;
	} else {
		if (!pskb_may_pull(skb, offset + sizeof(*mld2q))) {
			err = -EINVAL;
			goto out;
		}
		mld2q = (struct mld2_query *)icmp6_hdr(skb);
		if (!mld2q->mld2q_nsrcs)
			group = &mld2q->mld2q_mca;

		max_delay = max(msecs_to_jiffies(mldv2_mrc(mld2q)), 1UL);
	}

	is_general_query = group && ipv6_addr_any(group);

	if (is_general_query) {
		saddr.proto = htons(ETH_P_IPV6);
		saddr.u.ip6 = ip6h->saddr;

		br_multicast_query_received(br, port, &br->ip6_other_query,
					    &saddr, max_delay);
		goto out;
	} else if (!group) {
		goto out;
	}

	mp = br_mdb_ip6_get(mlock_dereference(br->mdb, br), group, vid);
	if (!mp)
		goto out;

	max_delay *= br->multicast_last_member_count;
	if (mp->mglist &&
	    (timer_pending(&mp->timer) ?
	     time_after(mp->timer.expires, now + max_delay) :
	     try_to_del_timer_sync(&mp->timer) >= 0))
		mod_timer(&mp->timer, now + max_delay);

	for (pp = &mp->ports;
	     (p = mlock_dereference(*pp, br)) != NULL;
	     pp = &p->next) {
		if (timer_pending(&p->timer) ?
		    time_after(p->timer.expires, now + max_delay) :
		    try_to_del_timer_sync(&p->timer) >= 0)
			mod_timer(&p->timer, now + max_delay);
	}

out:
	spin_unlock(&br->multicast_lock);
	return err;
}
#endif

static void
br_multicast_leave_group(struct net_bridge *br,
			 struct net_bridge_port *port,
			 struct br_ip *group,
			 struct bridge_mcast_other_query *other_query,
			 struct bridge_mcast_own_query *own_query)
{
	struct net_bridge_mdb_htable *mdb;
	struct net_bridge_mdb_entry *mp;
	struct net_bridge_port_group *p;
	unsigned long now;
	unsigned long time;

	spin_lock(&br->multicast_lock);
	if (!netif_running(br->dev) ||
	    (port && port->state == BR_STATE_DISABLED))
		goto out;

	mdb = mlock_dereference(br->mdb, br);
	mp = br_mdb_ip_get(mdb, group);
	if (!mp)
		goto out;

	if (port && (port->flags & BR_MULTICAST_FAST_LEAVE)) {
		struct net_bridge_port_group __rcu **pp;

		for (pp = &mp->ports;
		     (p = mlock_dereference(*pp, br)) != NULL;
		     pp = &p->next) {
			if (p->port != port)
				continue;

			rcu_assign_pointer(*pp, p->next);
			hlist_del_init(&p->mglist);
			del_timer(&p->timer);
			call_rcu_bh(&p->rcu, br_multicast_free_pg);
			br_mdb_notify(br->dev, port, group, RTM_DELMDB,
				      p->state);

			if (!mp->ports && !mp->mglist &&
			    netif_running(br->dev))
				mod_timer(&mp->timer, jiffies);
		}
		goto out;
	}

	if (timer_pending(&other_query->timer))
		goto out;

	if (br->multicast_querier) {
		__br_multicast_send_query(br, port, &mp->addr);

		time = jiffies + br->multicast_last_member_count *
				 br->multicast_last_member_interval;

		mod_timer(&own_query->timer, time);

		for (p = mlock_dereference(mp->ports, br);
		     p != NULL;
		     p = mlock_dereference(p->next, br)) {
			if (p->port != port)
				continue;

			if (!hlist_unhashed(&p->mglist) &&
			    (timer_pending(&p->timer) ?
			     time_after(p->timer.expires, time) :
			     try_to_del_timer_sync(&p->timer) >= 0)) {
				mod_timer(&p->timer, time);
			}

			break;
		}
	}

	now = jiffies;
	time = now + br->multicast_last_member_count *
		     br->multicast_last_member_interval;

	if (!port) {
		if (mp->mglist &&
		    (timer_pending(&mp->timer) ?
		     time_after(mp->timer.expires, time) :
		     try_to_del_timer_sync(&mp->timer) >= 0)) {
			mod_timer(&mp->timer, time);
		}

		goto out;
	}

	for (p = mlock_dereference(mp->ports, br);
	     p != NULL;
	     p = mlock_dereference(p->next, br)) {
		if (p->port != port)
			continue;

		if (!hlist_unhashed(&p->mglist) &&
		    (timer_pending(&p->timer) ?
		     time_after(p->timer.expires, time) :
		     try_to_del_timer_sync(&p->timer) >= 0)) {
			mod_timer(&p->timer, time);
		}

		break;
	}
out:
	spin_unlock(&br->multicast_lock);
}

static void br_ip4_multicast_leave_group(struct net_bridge *br,
					 struct net_bridge_port *port,
					 __be32 group,
					 __u16 vid)
{
	struct br_ip br_group;
	struct bridge_mcast_own_query *own_query;

	if (ipv4_is_local_multicast(group))
		return;

	own_query = port ? &port->ip4_own_query : &br->ip4_own_query;

	br_group.u.ip4 = group;
	br_group.proto = htons(ETH_P_IP);
	br_group.vid = vid;

	br_multicast_leave_group(br, port, &br_group, &br->ip4_other_query,
				 own_query);
}

#if IS_ENABLED(CONFIG_IPV6)
static void br_ip6_multicast_leave_group(struct net_bridge *br,
					 struct net_bridge_port *port,
					 const struct in6_addr *group,
					 __u16 vid)
{
	struct br_ip br_group;
	struct bridge_mcast_own_query *own_query;

	if (ipv6_addr_is_ll_all_nodes(group))
		return;

	own_query = port ? &port->ip6_own_query : &br->ip6_own_query;

	br_group.u.ip6 = *group;
	br_group.proto = htons(ETH_P_IPV6);
	br_group.vid = vid;

	br_multicast_leave_group(br, port, &br_group, &br->ip6_other_query,
				 own_query);
}
#endif

static int br_multicast_ipv4_rcv(struct net_bridge *br,
				 struct net_bridge_port *port,
				 struct sk_buff *skb,
				 u16 vid)
{
	struct sk_buff *skb_trimmed = NULL;
	struct igmphdr *ih;
	int err;

	err = ip_mc_check_igmp(skb, &skb_trimmed);

	if (err == -ENOMSG) {
		if (!ipv4_is_local_multicast(ip_hdr(skb)->daddr))
			BR_INPUT_SKB_CB(skb)->mrouters_only = 1;
		return 0;
	} else if (err < 0) {
		return err;
	}

	BR_INPUT_SKB_CB(skb)->igmp = 1;
	ih = igmp_hdr(skb);

	switch (ih->type) {
	case IGMP_HOST_MEMBERSHIP_REPORT:
	case IGMPV2_HOST_MEMBERSHIP_REPORT:
		BR_INPUT_SKB_CB(skb)->mrouters_only = 1;
		err = br_ip4_multicast_add_group(br, port, ih->group, vid);
#if defined(CONFIG_RTL_HARDWARE_MULTICAST)
		rtl865x_updateHwMulticast(br,ih->group);
#endif /* CONFIG_RTL_HARDWARE_MULTICAST */
		break;
	case IGMPV3_HOST_MEMBERSHIP_REPORT:
		err = br_ip4_multicast_igmp3_report(br, port, skb_trimmed, vid);
		break;
	case IGMP_HOST_MEMBERSHIP_QUERY:
		err = br_ip4_multicast_query(br, port, skb_trimmed, vid);
#if defined(CONFIG_RTL_HARDWARE_MULTICAST)
		rtl865x_updateHwMulticast(br,ih->group);
#endif /* CONFIG_RTL_HARDWARE_MULTICAST */
		break;
	case IGMP_HOST_LEAVE_MESSAGE:
		br_ip4_multicast_leave_group(br, port, ih->group, vid);
		break;
	}

	if (skb_trimmed && skb_trimmed != skb)
		kfree_skb(skb_trimmed);

	return err;
}

#if IS_ENABLED(CONFIG_IPV6)
static int br_multicast_ipv6_rcv(struct net_bridge *br,
				 struct net_bridge_port *port,
				 struct sk_buff *skb,
				 u16 vid)
{
	struct sk_buff *skb_trimmed = NULL;
	struct mld_msg *mld;
	int err;

	err = ipv6_mc_check_mld(skb, &skb_trimmed);

	if (err == -ENOMSG) {
		if (!ipv6_addr_is_ll_all_nodes(&ipv6_hdr(skb)->daddr))
			BR_INPUT_SKB_CB(skb)->mrouters_only = 1;
		return 0;
	} else if (err < 0) {
		return err;
	}

	BR_INPUT_SKB_CB(skb)->igmp = 1;
	mld = (struct mld_msg *)skb_transport_header(skb);

	switch (mld->mld_type) {
	case ICMPV6_MGM_REPORT:
		BR_INPUT_SKB_CB(skb)->mrouters_only = 1;
		err = br_ip6_multicast_add_group(br, port, &mld->mld_mca, vid);
		break;
	case ICMPV6_MLD2_REPORT:
		err = br_ip6_multicast_mld2_report(br, port, skb_trimmed, vid);
		break;
	case ICMPV6_MGM_QUERY:
		err = br_ip6_multicast_query(br, port, skb_trimmed, vid);
		break;
	case ICMPV6_MGM_REDUCTION:
		br_ip6_multicast_leave_group(br, port, &mld->mld_mca, vid);
		break;
	}

	if (skb_trimmed && skb_trimmed != skb)
		kfree_skb(skb_trimmed);

	return err;
}
#endif

int br_multicast_rcv(struct net_bridge *br, struct net_bridge_port *port,
		     struct sk_buff *skb, u16 vid)
{
	BR_INPUT_SKB_CB(skb)->igmp = 0;
	BR_INPUT_SKB_CB(skb)->mrouters_only = 0;

	if (br->multicast_disabled)
		return 0;
#if defined(MCAST_TO_UNICAST)
	char tmpOp;
	unsigned int moreFlag = 1;
	unsigned char macAddr[6];
	unsigned char operation;
	unsigned int gIndex = 0;
	struct iphdr *iph = NULL;
	const unsigned char *dest = eth_hdr(skb)->h_dest;
	struct net_bridge_port *p = br_port_get_rcu(skb->dev);
	unsigned char proto = 0;
	if (!(br->dev->flags & IFF_PROMISC)
		&& MULTICAST_MAC(dest)
		&& (eth_hdr(skb)->h_proto == htons(ETH_P_IP)))
	{
		iph = (struct iphdr *)skb_network_header(skb);
		proto = iph->protocol;
		if (proto == IPPROTO_IGMP)
		{
			while (moreFlag)
			{
				tmpOp = igmp_type_check(skb, macAddr, &gIndex, &moreFlag);
				if (tmpOp > 0)
				{
					operation = (unsigned char)tmpOp;
					br_update_igmp_snoop_fdb(operation, br, p, macAddr, skb);
				}
			}
		}
	}
	else if (!(br->dev->flags & IFF_PROMISC)
		&& IPV6_MULTICAST_MAC(dest)
		&& (eth_hdr(skb)->h_proto == ETH_P_IPV6))
	{
#if defined(IPV6_MCAST_TO_UNICAST)
		tmpOp = ICMPv6_check(skb , macAddr);
		if (tmpOp > 0){
			operation = (unsigned char)tmpOp;
#ifdef DBG_ICMPv6
		if (operation == 1)
			printk("icmpv6 add from frame finish\n");
		else if (operation == 2)
			printk("icmpv6 del from frame finish\n");
#endif /* DBG_ICMPv6 */
			br_update_igmp_snoop_fdb(operation, br, p, macAddr, skb);
		}
	}
#endif /* IPV6_MCAST_TO_UNICAST */
#endif /* MULTICAST_TO_UNICAST */

	switch (skb->protocol) {
	case htons(ETH_P_IP):
		return br_multicast_ipv4_rcv(br, port, skb, vid);
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		return br_multicast_ipv6_rcv(br, port, skb, vid);
#endif
	}

	return 0;
}

static void br_multicast_query_expired(struct net_bridge *br,
				       struct bridge_mcast_own_query *query,
				       struct bridge_mcast_querier *querier)
{
	spin_lock(&br->multicast_lock);
	if (query->startup_sent < br->multicast_startup_query_count)
		query->startup_sent++;

	RCU_INIT_POINTER(querier->port, NULL);
	br_multicast_send_query(br, NULL, query);
	spin_unlock(&br->multicast_lock);
}

static void br_ip4_multicast_query_expired(unsigned long data)
{
	struct net_bridge *br = (void *)data;

	br_multicast_query_expired(br, &br->ip4_own_query, &br->ip4_querier);
}

#if IS_ENABLED(CONFIG_IPV6)
static void br_ip6_multicast_query_expired(unsigned long data)
{
	struct net_bridge *br = (void *)data;

	br_multicast_query_expired(br, &br->ip6_own_query, &br->ip6_querier);
}
#endif

void br_multicast_init(struct net_bridge *br)
{
	br->hash_elasticity = 4;
	br->hash_max = 512;

	br->multicast_router = 1;
	br->multicast_querier = 0;
	br->multicast_query_use_ifaddr = 0;
	br->multicast_last_member_count = 2;
	br->multicast_startup_query_count = 2;

	br->multicast_last_member_interval = HZ;
	br->multicast_query_response_interval = 10 * HZ;
	br->multicast_startup_query_interval = 125 * HZ / 4;
	br->multicast_query_interval = 125 * HZ;
	br->multicast_querier_interval = 255 * HZ;
	br->multicast_membership_interval = 260 * HZ;

	br->ip4_other_query.delay_time = 0;
	br->ip4_querier.port = NULL;
#if IS_ENABLED(CONFIG_IPV6)
	br->ip6_other_query.delay_time = 0;
	br->ip6_querier.port = NULL;
#endif
	br->has_ipv6_addr = 1;

	spin_lock_init(&br->multicast_lock);
	setup_timer(&br->multicast_router_timer,
		    br_multicast_local_router_expired, 0);
	setup_timer(&br->ip4_other_query.timer,
		    br_ip4_multicast_querier_expired, (unsigned long)br);
	setup_timer(&br->ip4_own_query.timer, br_ip4_multicast_query_expired,
		    (unsigned long)br);
#if IS_ENABLED(CONFIG_IPV6)
	setup_timer(&br->ip6_other_query.timer,
		    br_ip6_multicast_querier_expired, (unsigned long)br);
	setup_timer(&br->ip6_own_query.timer, br_ip6_multicast_query_expired,
		    (unsigned long)br);
#endif
}

static void __br_multicast_open(struct net_bridge *br,
				struct bridge_mcast_own_query *query)
{
	query->startup_sent = 0;

	if (br->multicast_disabled)
		return;

	mod_timer(&query->timer, jiffies);
}

void br_multicast_open(struct net_bridge *br)
{
	__br_multicast_open(br, &br->ip4_own_query);
#if IS_ENABLED(CONFIG_IPV6)
	__br_multicast_open(br, &br->ip6_own_query);
#endif
}

void br_multicast_stop(struct net_bridge *br)
{
	del_timer_sync(&br->multicast_router_timer);
	del_timer_sync(&br->ip4_other_query.timer);
	del_timer_sync(&br->ip4_own_query.timer);
#if IS_ENABLED(CONFIG_IPV6)
	del_timer_sync(&br->ip6_other_query.timer);
	del_timer_sync(&br->ip6_own_query.timer);
#endif
}

void br_multicast_dev_del(struct net_bridge *br)
{
	struct net_bridge_mdb_htable *mdb;
	struct net_bridge_mdb_entry *mp;
	struct hlist_node *n;
	u32 ver;
	int i;

	spin_lock_bh(&br->multicast_lock);
	mdb = mlock_dereference(br->mdb, br);
	if (!mdb)
		goto out;

	br->mdb = NULL;

	ver = mdb->ver;
	for (i = 0; i < mdb->max; i++) {
		hlist_for_each_entry_safe(mp, n, &mdb->mhash[i],
					  hlist[ver]) {
			del_timer(&mp->timer);
			call_rcu_bh(&mp->rcu, br_multicast_free_group);
		}
	}

	if (mdb->old) {
		spin_unlock_bh(&br->multicast_lock);
		rcu_barrier_bh();
		spin_lock_bh(&br->multicast_lock);
		WARN_ON(mdb->old);
	}

	mdb->old = mdb;
	call_rcu_bh(&mdb->rcu, br_mdb_free);

out:
	spin_unlock_bh(&br->multicast_lock);
}

int br_multicast_set_router(struct net_bridge *br, unsigned long val)
{
	int err = -EINVAL;

	spin_lock_bh(&br->multicast_lock);

	switch (val) {
	case 0:
	case 2:
		del_timer(&br->multicast_router_timer);
		/* fall through */
	case 1:
		br->multicast_router = val;
		err = 0;
		break;
	}

	spin_unlock_bh(&br->multicast_lock);

	return err;
}

int br_multicast_set_port_router(struct net_bridge_port *p, unsigned long val)
{
	struct net_bridge *br = p->br;
	int err = -EINVAL;

	spin_lock(&br->multicast_lock);

	switch (val) {
	case 0:
	case 1:
	case 2:
		p->multicast_router = val;
		err = 0;

		if (val < 2 && !hlist_unhashed(&p->rlist)) {
			hlist_del_init_rcu(&p->rlist);
			br_rtr_notify(br->dev, p, RTM_DELMDB);
		}

		if (val == 1)
			break;

		del_timer(&p->multicast_router_timer);

		if (val == 0)
			break;

		br_multicast_add_router(br, p);
		break;
	}

	spin_unlock(&br->multicast_lock);

	return err;
}

static void br_multicast_start_querier(struct net_bridge *br,
				       struct bridge_mcast_own_query *query)
{
	struct net_bridge_port *port;

	__br_multicast_open(br, query);

	list_for_each_entry(port, &br->port_list, list) {
		if (port->state == BR_STATE_DISABLED ||
		    port->state == BR_STATE_BLOCKING)
			continue;

		if (query == &br->ip4_own_query)
			br_multicast_enable(&port->ip4_own_query);
#if IS_ENABLED(CONFIG_IPV6)
		else
			br_multicast_enable(&port->ip6_own_query);
#endif
	}
}

int br_multicast_toggle(struct net_bridge *br, unsigned long val)
{
	int err = 0;
	struct net_bridge_mdb_htable *mdb;

	spin_lock_bh(&br->multicast_lock);
	if (br->multicast_disabled == !val)
		goto unlock;

	br->multicast_disabled = !val;
	if (br->multicast_disabled)
		goto unlock;

	if (!netif_running(br->dev))
		goto unlock;

	mdb = mlock_dereference(br->mdb, br);
	if (mdb) {
		if (mdb->old) {
			err = -EEXIST;
rollback:
			br->multicast_disabled = !!val;
			goto unlock;
		}

		err = br_mdb_rehash(&br->mdb, mdb->max,
				    br->hash_elasticity);
		if (err)
			goto rollback;
	}

	br_multicast_start_querier(br, &br->ip4_own_query);
#if IS_ENABLED(CONFIG_IPV6)
	br_multicast_start_querier(br, &br->ip6_own_query);
#endif

unlock:
	spin_unlock_bh(&br->multicast_lock);

	return err;
}

int br_multicast_set_querier(struct net_bridge *br, unsigned long val)
{
	unsigned long max_delay;

	val = !!val;

	spin_lock_bh(&br->multicast_lock);
	if (br->multicast_querier == val)
		goto unlock;

	br->multicast_querier = val;
	if (!val)
		goto unlock;

	max_delay = br->multicast_query_response_interval;

	if (!timer_pending(&br->ip4_other_query.timer))
		br->ip4_other_query.delay_time = jiffies + max_delay;

	br_multicast_start_querier(br, &br->ip4_own_query);

#if IS_ENABLED(CONFIG_IPV6)
	if (!timer_pending(&br->ip6_other_query.timer))
		br->ip6_other_query.delay_time = jiffies + max_delay;

	br_multicast_start_querier(br, &br->ip6_own_query);
#endif

unlock:
	spin_unlock_bh(&br->multicast_lock);

	return 0;
}

int br_multicast_set_hash_max(struct net_bridge *br, unsigned long val)
{
	int err = -EINVAL;
	u32 old;
	struct net_bridge_mdb_htable *mdb;

	spin_lock_bh(&br->multicast_lock);
	if (!is_power_of_2(val))
		goto unlock;

	mdb = mlock_dereference(br->mdb, br);
	if (mdb && val < mdb->size)
		goto unlock;

	err = 0;

	old = br->hash_max;
	br->hash_max = val;

	if (mdb) {
		if (mdb->old) {
			err = -EEXIST;
rollback:
			br->hash_max = old;
			goto unlock;
		}

		err = br_mdb_rehash(&br->mdb, br->hash_max,
				    br->hash_elasticity);
		if (err)
			goto rollback;
	}

unlock:
	spin_unlock_bh(&br->multicast_lock);

	return err;
}

/**
 * br_multicast_list_adjacent - Returns snooped multicast addresses
 * @dev:	The bridge port adjacent to which to retrieve addresses
 * @br_ip_list:	The list to store found, snooped multicast IP addresses in
 *
 * Creates a list of IP addresses (struct br_ip_list) sensed by the multicast
 * snooping feature on all bridge ports of dev's bridge device, excluding
 * the addresses from dev itself.
 *
 * Returns the number of items added to br_ip_list.
 *
 * Notes:
 * - br_ip_list needs to be initialized by caller
 * - br_ip_list might contain duplicates in the end
 *   (needs to be taken care of by caller)
 * - br_ip_list needs to be freed by caller
 */
int br_multicast_list_adjacent(struct net_device *dev,
			       struct list_head *br_ip_list)
{
	struct net_bridge *br;
	struct net_bridge_port *port;
	struct net_bridge_port_group *group;
	struct br_ip_list *entry;
	int count = 0;

	rcu_read_lock();
	if (!br_ip_list || !br_port_exists(dev))
		goto unlock;

	port = br_port_get_rcu(dev);
	if (!port || !port->br)
		goto unlock;

	br = port->br;

	list_for_each_entry_rcu(port, &br->port_list, list) {
		if (!port->dev || port->dev == dev)
			continue;

		hlist_for_each_entry_rcu(group, &port->mglist, mglist) {
			entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
			if (!entry)
				goto unlock;

			entry->addr = group->addr;
			list_add(&entry->list, br_ip_list);
			count++;
		}
	}

unlock:
	rcu_read_unlock();
	return count;
}
EXPORT_SYMBOL_GPL(br_multicast_list_adjacent);

/**
 * br_multicast_has_querier_anywhere - Checks for a querier on a bridge
 * @dev: The bridge port providing the bridge on which to check for a querier
 * @proto: The protocol family to check for: IGMP -> ETH_P_IP, MLD -> ETH_P_IPV6
 *
 * Checks whether the given interface has a bridge on top and if so returns
 * true if a valid querier exists anywhere on the bridged link layer.
 * Otherwise returns false.
 */
bool br_multicast_has_querier_anywhere(struct net_device *dev, int proto)
{
	struct net_bridge *br;
	struct net_bridge_port *port;
	struct ethhdr eth;
	bool ret = false;

	rcu_read_lock();
	if (!br_port_exists(dev))
		goto unlock;

	port = br_port_get_rcu(dev);
	if (!port || !port->br)
		goto unlock;

	br = port->br;

	memset(&eth, 0, sizeof(eth));
	eth.h_proto = htons(proto);

	ret = br_multicast_querier_exists(br, &eth);

unlock:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(br_multicast_has_querier_anywhere);

/**
 * br_multicast_has_querier_adjacent - Checks for a querier behind a bridge port
 * @dev: The bridge port adjacent to which to check for a querier
 * @proto: The protocol family to check for: IGMP -> ETH_P_IP, MLD -> ETH_P_IPV6
 *
 * Checks whether the given interface has a bridge on top and if so returns
 * true if a selected querier is behind one of the other ports of this
 * bridge. Otherwise returns false.
 */
bool br_multicast_has_querier_adjacent(struct net_device *dev, int proto)
{
	struct net_bridge *br;
	struct net_bridge_port *port;
	bool ret = false;

	rcu_read_lock();
	if (!br_port_exists(dev))
		goto unlock;

	port = br_port_get_rcu(dev);
	if (!port || !port->br)
		goto unlock;

	br = port->br;

	switch (proto) {
	case ETH_P_IP:
		if (!timer_pending(&br->ip4_other_query.timer) ||
		    rcu_dereference(br->ip4_querier.port) == port)
			goto unlock;
		break;
#if IS_ENABLED(CONFIG_IPV6)
	case ETH_P_IPV6:
		if (!timer_pending(&br->ip6_other_query.timer) ||
		    rcu_dereference(br->ip6_querier.port) == port)
			goto unlock;
		break;
#endif
	default:
		goto unlock;
	}

	ret = true;
unlock:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(br_multicast_has_querier_adjacent);

#if defined(CONFIG_BRIDGE_IGMP_SNOOPING) && defined(CONFIG_RTL_HARDWARE_MULTICAST)

extern int32 rtl865x_multicastUpdate(rtl865x_mcast_fwd_descriptor_t* desc);
extern uint32 rtl_getDevPortMaskByName(struct net_device *dev);

int32 rtl865x_getMcastFwdInfo(struct net_bridge_mdb_entry *mdst,
			      struct multicastFwdInfo *mcastFwdInfo)
{
	if (mdst == NULL || mcastFwdInfo == NULL)
	{
		#ifdef HW_MULTICAST_DBG
		printk("[%s:%d]mdst==NULL||descriptor==NULL\n", __FUNCTION__, __LINE__);
		#endif /* HW_MULTICAST_DBG */
		return FAILED;
	}
	memset(mcastFwdInfo, 0, sizeof(struct multicastFwdInfo));
	mcastFwdInfo->fwdPortMask = 0;
	mcastFwdInfo->toCpu = FALSE;
	struct net_bridge_port_group *p;
	p = rcu_dereference(mdst->ports);
	if (p == NULL)
	{
		#ifdef HW_MULTICAST_DBG
		printk("[%s:%d]p==NULL\n", __FUNCTION__, __LINE__);
		#endif /* HW_MULTICAST_DBG */
		return FAILED;
	}
	while (p)
	{
		struct dev_priv *devPriv = (struct dev_priv *)netdev_priv(p->port->dev);
#ifdef HW_MULTICAST_DBG
		printk("%s:port number is %u,phy port number is %u\n", __FUNCTION__, p->port->port_no, devPriv->portmask);
#endif /* HW_MULTICAST_DBG */
		mcastFwdInfo->fwdPortMask |= devPriv->portmask;
		if (memcmp(devPriv->dev->name, RTL_PS_WLAN0_DEV_NAME, 5) == 0)
			mcastFwdInfo->toCpu = TRUE;
		p = p->next;
	}
	return SUCCESS;
}

int32 rtl865x_updateHwMulticast(struct net_bridge *br,
				__be32 group)
{
	struct net_bridge_mdb_htable *mdb;
	rtl865x_mcast_fwd_descriptor_t desc;
	struct multicastFwdInfo mcastFwdInfo;
	struct br_ip mAddr;
	memset(&desc, 0, sizeof(rtl865x_mcast_fwd_descriptor_t));
	strcpy(desc.netifName, RTL_PS_BR0_DEV_NAME);
#ifdef HW_MULTICAST_DBG
	printk("[%s:%d]group:0x%x\n", __FUNCTION__, __LINE__, group);
#endif /* HW_MULTICAST_DBG */
	if (br == NULL)
	{
		#ifdef HW_MULTICAST_DBG
		printk("[%s:%d]br==NULL\n", __FUNCTION__, __LINE__);
		#endif /* HW_MULTICAST_DBG */
		goto err;
	}

	mdb = rcu_dereference(br->mdb);
	if (mdb == NULL)
	{
		#ifdef HW_MULTICAST_DBG
		printk("[%s:%d]mdb==NULL\n", __FUNCTION__, __LINE__);
		goto err;
		#endif /* HW_MULTICAST_DBG */
	}
	mAddr.proto = htons(ETH_P_IP);
	mAddr.u.ip4 = group;
	struct net_bridge_mdb_entry *mdst = br_mdb_ip_get(mdb, &mAddr);
	if (mdst == NULL)
	{
#ifdef HW_MULTICAST_DBG
		printk("[%s:%d]mdst==NULL\n", __FUNCTION__, __LINE__);
#endif /* HW_MULTICAST_DBG */
		goto err;
	}
	uint32 retVal = rtl865x_getMcastFwdInfo(mdst,&mcastFwdInfo);
	if (retVal == SUCCESS)
	{
		desc.dip = group;
		desc.toCpu = mcastFwdInfo.toCpu;
		desc.fwdPortMask = mcastFwdInfo.fwdPortMask;
		return rtl865x_multicastUpdate(&desc);
	}
err:
	desc.toCpu = FALSE;
	desc.dip = group;
	desc.fwdPortMask = 0;
	return rtl865x_multicastUpdate(&desc);
}


#if defined(MCAST_TO_UNICAST)
static void ConvertMulticatIPtoMacAddr(__u32 group, unsigned char *gmac)
{
	__u32 u32tmp, tmp;
	int i;

	u32tmp = group & 0x007FFFFF;
	gmac[0]=0x01;
	gmac[1]=0x00;
	gmac[2]=0x5e;
	for (i = 5; i >= 3; i--) {
		tmp = u32tmp & 0xFF;
		gmac[i] = tmp;
		u32tmp >>= 8;
	}
}
static char igmp_type_check(struct sk_buff *skb, unsigned char *gmac,unsigned int *gIndex,unsigned int *moreFlag)
{
	struct iphdr *iph;
	__u8 hdrlen;
	struct igmphdr *igmph;
	int i;
	unsigned int groupAddr = 0;// add  for fit igmp v3
	*moreFlag = 0;
	/* check IP header information */
	iph = (struct iphdr *)skb_network_header(skb);
	hdrlen = iph->ihl << 2;
	if ((iph->version != 4) && (hdrlen < 20))
		return -1;
	if (ip_fast_csum((u8 *)iph, iph->ihl) != 0)
		return -1;
	{ /* check the length */
		__u32 len = ntohs(iph->tot_len);
		if (skb->len < len || len < hdrlen)
			return -1;
	}
	/* parsing the igmp packet */
	igmph = (struct igmphdr *)((u8*)iph + hdrlen);
	if ((igmph->type == IGMP_HOST_MEMBERSHIP_REPORT) ||
	    (igmph->type == IGMPV2_HOST_MEMBERSHIP_REPORT))
	{
		groupAddr = igmph->group;
		if (!IN_MULTICAST(groupAddr))
		{
				return -1;
		}

		ConvertMulticatIPtoMacAddr(groupAddr, gmac);

		return 1; /* report and add it */
	}
	else if (igmph->type == IGMPV3_HOST_MEMBERSHIP_REPORT) {


		/*for support igmp v3 ; plusWang add 2009-0311*/
		struct igmpv3_report *igmpv3report = (struct igmpv3_report * )igmph;
		struct igmpv3_grec *igmpv3grec = NULL;

		if (*gIndex >= igmpv3report->ngrec)
		{
			*moreFlag = 0;
			return -1;
		}

		for (i = 0;i < igmpv3report->ngrec; i++)
		{

			if (i == 0)
			{
				igmpv3grec = (struct igmpv3_grec *)(&(igmpv3report->grec)); /*first igmp group record*/
			}
			else
			{
				igmpv3grec = (struct igmpv3_grec *)((unsigned char*)igmpv3grec + 8 + igmpv3grec->grec_nsrcs * 4 + (igmpv3grec->grec_auxwords) * 4);


			}

			if (i != *gIndex)
			{
				continue;
			}

			if (i == (igmpv3report->ngrec - 1))
			{
				/*last group record*/
				*moreFlag = 0;
			}
			else
			{
				*moreFlag = 1;
			}

			/*gIndex move to next group*/
			*gIndex = *gIndex + 1;

			groupAddr = igmpv3grec->grec_mca;
			if (!IN_MULTICAST(groupAddr))
			{
				return -1;
			}

			ConvertMulticatIPtoMacAddr(groupAddr, gmac);
			if (((igmpv3grec->grec_type == IGMPV3_CHANGE_TO_INCLUDE) || (igmpv3grec->grec_type == IGMPV3_MODE_IS_INCLUDE)) && (igmpv3grec->grec_nsrcs == 0))
			{
				return 2; /* leave and delete it */
			}
			else if ((igmpv3grec->grec_type == IGMPV3_CHANGE_TO_EXCLUDE) ||
				(igmpv3grec->grec_type == IGMPV3_MODE_IS_EXCLUDE) ||
				(igmpv3grec->grec_type == IGMPV3_ALLOW_NEW_SOURCES))
			{
				return 1;
			}
			else
			{
				/*ignore it*/
			}

			return -1;
		}

		/*avoid dead loop in case of initial gIndex is too big*/
		if (i >= (igmpv3report->ngrec - 1))
		{
			/*last group record*/
			*moreFlag = 0;
			return -1;
		}


	}
	else if (igmph->type == IGMP_HOST_LEAVE_MESSAGE){

		groupAddr = igmph->group;
		if (!IN_MULTICAST(groupAddr))
		{
				return -1;
		}

		ConvertMulticatIPtoMacAddr(groupAddr, gmac);
		return 2; /* leave and delete it */
	}


	return -1;
}


#if defined(IPV6_MCAST_TO_UNICAST)
static void CIPV6toMac(unsigned char* icmpv6_McastAddr, unsigned char *gmac)
{
	/*ICMPv6 valid addr 2^32 -1*/
	gmac[0] = 0x33;
	gmac[1] = 0x33;
	gmac[2] = icmpv6_McastAddr[12];
	gmac[3] = icmpv6_McastAddr[13];
	gmac[4] = icmpv6_McastAddr[14];
	gmac[5] = icmpv6_McastAddr[15];
}
static char ICMPv6_check(struct sk_buff *skb , unsigned char *gmac)
{

	struct ipv6hdr *ipv6h;
	char* protoType;

	/* check IPv6 header information */
	ipv6h = (struct ipv6hdr *)skb_network_header(skb);
	if (ipv6h->version != 6) {
		return -1;
	}


	/*Next header: IPv6 hop-by-hop option (0x00)*/
	if (ipv6h->nexthdr == 0) {
		protoType = (unsigned char*)((unsigned char*)ipv6h + sizeof(struct ipv6hdr));
	}else{
		return -1;
	}

	if (protoType[0] == 0x3a) {

		struct icmp6hdr *icmpv6h = (struct icmp6hdr*)(protoType + 8);
		unsigned char *icmpv6_McastAddr;

		if (icmpv6h->icmp6_type == 0x83) {

			icmpv6_McastAddr = (unsigned char*)((unsigned char*)icmpv6h + 8);
			#ifdef DBG_ICMPv6
			printk("Type: 0x%x (Multicast listener report) \n", icmpv6h->icmp6_type);
			#endif /* DBG_ICMPv6 */

		}else if (icmpv6h->icmp6_type == 0x8f) {

			icmpv6_McastAddr = (unsigned char*)((unsigned char*)icmpv6h + 8 + 4);
			#ifdef DBG_ICMPv6
			printk("Type: 0x%x (Multicast listener report v2) \n", icmpv6h->icmp6_type);
			#endif /* DBG_ICMPv6 */
		}else if (icmpv6h->icmp6_type == 0x84) {

			icmpv6_McastAddr = (unsigned char*)((unsigned char*)icmpv6h + 8);
			#ifdef DBG_ICMPv6
			printk("Type: 0x%x (Multicast listener done ) \n", icmpv6h->icmp6_type);
			#endif /* DBG_ICMPv6 */
		}
		else {
			#ifdef DBG_ICMPv6
			printk("Type: 0x%x (unknow type)\n", icmpv6h->icmp6_type);
			#endif /* DBG_ICMPv6 */
			return -1;
		}

		#ifdef	DBG_ICMPv6
		printk("MCAST_IPV6Addr:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x \n",
			icmpv6_McastAddr[0], icmpv6_McastAddr[1], icmpv6_McastAddr[2], icmpv6_McastAddr[3],
			icmpv6_McastAddr[4], icmpv6_McastAddr[5], icmpv6_McastAddr[6], icmpv6_McastAddr[7],
			icmpv6_McastAddr[8], icmpv6_McastAddr[9], icmpv6_McastAddr[10], icmpv6_McastAddr[11],
			icmpv6_McastAddr[12], icmpv6_McastAddr[13], icmpv6_McastAddr[14], icmpv6_McastAddr[15]);
		#endif /* DBG_ICMPv6 */

		CIPV6toMac(icmpv6_McastAddr, gmac);

		#ifdef	DBG_ICMPv6
		printk("group_mac [%02x:%02x:%02x:%02x:%02x:%02x] \n",
			gmac[0], gmac[1], gmac[2],
			gmac[3], gmac[4], gmac[5]);
		#endif /* DBG_ICMPv6 */



		if (icmpv6h->icmp6_type == 0x83) {

			return 1;//icmpv6 listener report (add)
		}
		else if (icmpv6h->icmp6_type == 0x8f) {
			return 1;//icmpv6 listener report v2 (add)
		}
		else if (icmpv6h->icmp6_type == 0x84) {
			return 2;//icmpv6 Multicast listener done (del)
		}
	}
	else {
		return -1;//not icmpv6 type
	}
	return -1;
}

#endif /* IPV6_MCAST_TO_UNICAST */
extern int chk_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac);
extern void add_igmp_ext_entry(struct net_bridge_fdb_entry *fdb , unsigned char *srcMac , unsigned char portComeIn);
extern void update_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac , unsigned char portComeIn);
extern void del_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac , unsigned char portComeIn, unsigned char expireFlag);

static void br_update_igmp_snoop_fdb(unsigned char op, struct net_bridge *br, struct net_bridge_port *p, unsigned char *dest
				     , struct sk_buff *skb)
{
#if defined(HW_MULTICAST_DBG)
	printk("[%s:%d]\n", __FUNCTION__, __LINE__);
#endif /* HW_MULTICAST_DBG */
	struct net_bridge_fdb_entry *dst;
	unsigned char *src;
	unsigned short del_group_src = 0;
	unsigned char port_comein;
	int tt1;
	u16 vid = 0;

#if defined(MCAST_TO_UNICAST)
	struct net_device *dev;
	if (!dest)
		return;
	if (!MULTICAST_MAC(dest)
#if defined(IPV6_MCAST_TO_UNICAST)
		&& !IPV6_MULTICAST_MAC(dest)
#endif /* IPV6_MCAST_TO_UNICAST */
	)
	{
		return;
	}
#endif /* MCAST_TO_UNICAST */

#if defined(CONFIG_RTL_HARDWARE_MULTICAST)

	if (skb->srcPort != 0xFFFF)
	{
		port_comein = 1 << skb->srcPort;
	}
	else
	{
		port_comein = 0x80;
	}

#else
	if (p && p->dev && p->dev->name && !memcmp(p->dev->name, RTL_PS_LAN_P0_DEV_NAME, 4))
	{
		port_comein = 0x01;
	}

	if (p && p->dev && p->dev->name && !memcmp(p->dev->name, RTL_PS_WLAN_NAME, 4))
	{
		port_comein = 0x80;
	}

#endif /* CONFIG_RTL_HARDWARE_MULTICAST */
	br_vlan_get_tag(skb, &vid);
	src = (unsigned char*)(skb_mac_header(skb) + ETH_ALEN);
	/* check whether entry exist */
	dst = __br_fdb_get(br, dest, vid);

	if (op == 1) /* add */
	{
#if defined(HW_MULTICAST_DBG)
		printk("[%s:%d]Add.......\n", __FUNCTION__, __LINE__);
#endif /* HW_MULTICAST_DBG */

		if (dst == NULL) {
			/* insert one fdb entry */
			DEBUG_PRINT("insert one fdb entry\n");
			br_fdb_insert(br, p, dest, vid);
			dst = __br_fdb_get(br, dest, vid);
			if (dst != NULL)
			{
				dst->igmpFlag = 1;
				dst->is_local = 0;
				dst->portlist = port_comein;
				dst->group_src = dst->group_src | (1 << p->port_no);
			}
		}

		if (dst) {
			dst->group_src = dst->group_src | (1 << p->port_no);
			dst->updated = jiffies;
			tt1 = chk_igmp_ext_entry(dst , src);
			if (tt1 == 0)
			{
				add_igmp_ext_entry(dst , src , port_comein);
			}
			else
			{
				update_igmp_ext_entry(dst , src , port_comein);
			}


			#if defined(MCAST_TO_UNICAST)
			/*process wlan client join --- start*/
			if (p && p->dev && p->dev->name && !memcmp(p->dev->name, RTL_PS_WLAN_NAME, 4))
			{
				dst->portlist |= 0x80;
				port_comein = 0x80;
				dev = p->dev;
				if (dev)
				{
					unsigned char StaMacAndGroup[20];
					memcpy(StaMacAndGroup, dest, 6);
					memcpy(StaMacAndGroup + 6, src, 6);
				#if defined(CONFIG_COMPAT_NET_DEV_OPS)
					if (dev->do_ioctl != NULL)
					{
						dev->do_ioctl(dev, (struct ifreq*)StaMacAndGroup, 0x8B80);
						DEBUG_PRINT*("... add to wlan mcast table:  DA:%02x:%02x:%02x:%02x:%02x:%02x ; SA:%02x:%02x:%02x:%02x:%02x:%02x\n",
							StaMacAndGroup[0], StaMacAndGroup[1], StaMacAndGroup[2], StaMacAndGroup[3], StaMacAndGroup[4], StaMacAndGroup[5],
							StaMacAndGroup[6], StaMacAndGroup[7], StaMacAndGroup[8], StaMacAndGroup[9], StaMacAndGroup[10], StaMacAndGroup[11]);
					}
				#else
					if (dev->netdev_ops->ndo_do_ioctl != NULL)
					{
						dev->netdev_ops->ndo_do_ioctl(dev, (struct ifreq*)StaMacAndGroup, 0x8B80);
						DEBUG_PRINT("... add to wlan mcast table:  DA:%02x:%02x:%02x:%02x:%02x:%02x ; SA:%02x:%02x:%02x:%02x:%02x:%02x\n",
							StaMacAndGroup[0], StaMacAndGroup[1], StaMacAndGroup[2], StaMacAndGroup[3], StaMacAndGroup[4], StaMacAndGroup[5],
							StaMacAndGroup[6], StaMacAndGroup[7], StaMacAndGroup[8], StaMacAndGroup[9], StaMacAndGroup[10], StaMacAndGroup[11]);

					}
				#endif /* CONFIG_COMPAT_NET_DEV_OPS */
				}
			}
			/*process wlan client join --- end*/
			#endif /* MCAST_TO_UNICAST */
		}
	}
	else if (op == 2 && dst) /* delete */
	{
		DEBUG_PRINT("dst->group_src = %x change to ", dst->group_src);
		del_group_src = ~(1 << p->port_no);
		dst->group_src = dst->group_src & del_group_src;
		DEBUG_PRINT(" %x ; p->port_no=%x \n", dst->group_src , p->port_no);

		/*process wlan client leave --- start*/
		if (p && p->dev && p->dev->name && !memcmp(p->dev->name, RTL_PS_WLAN_NAME, 4))
		{
			port_comein = 0x80;
		}
		/*process wlan client leave --- end*/

		/*process entry del , portlist update*/
		del_igmp_ext_entry(dst, src , port_comein, 0);

		if (dst->portlist == 0)  // all joined sta are gone
		{
			DEBUG_PRINT("----all joined sta are gone,make it expired----\n");
			dst->updated -=  300 * HZ; // make it expired
		}


	}
}

#endif/*END OF MCAST_TO_UNICAST*/
#endif/*END OF CONFIG_RTL_HARDWARE_MULTICAST*/
