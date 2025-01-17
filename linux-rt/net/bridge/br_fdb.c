/*
 *	Forwarding database
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include <linux/times.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <asm/unaligned.h>
#include <linux/if_vlan.h>
#include <net/switchdev.h>
#include "br_private.h"
#if defined(CONFIG_RTL_819X)
#include <linux/wireless.h>

#include <net/rtl/features/rtl_ps_hooks.h>

static int fdb_entry_max = 2048;
static int fdb_entry_num = 0;
#endif /* defined(CONFIG_RTL_819X) */

#if defined(CONFIG_RTL_IGMP_SNOOPING)
#include <net/rtl/rtl865x_netif.h>
#include <net/rtl/rtl_nic.h>
extern int IGMPProxyOpened;
void add_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac , unsigned char portComeIn);
void update_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac , unsigned char portComeIn);
void del_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac , unsigned char portComeIn , unsigned char expireFlag);
#ifdef M2U_DELETE_CHECK
extern int rtl_M2UDeletecheck(unsigned char *dMac, unsigned char *sMac);
#endif
#endif /* CONFIG_RTL_IGMP_SNOOPING */


static struct kmem_cache *br_fdb_cache __read_mostly;
static struct net_bridge_fdb_entry *fdb_find(struct hlist_head *head,
					     const unsigned char *addr,
					     __u16 vid);
static int fdb_insert(struct net_bridge *br, struct net_bridge_port *source,
		      const unsigned char *addr, u16 vid);
static void fdb_notify(struct net_bridge *br,
		       const struct net_bridge_fdb_entry *, int);

static u32 fdb_salt __read_mostly;

int __init br_fdb_init(void)
{
	br_fdb_cache = kmem_cache_create("bridge_fdb_cache",
					 sizeof(struct net_bridge_fdb_entry),
					 0,
					 SLAB_HWCACHE_ALIGN, NULL);
	if (!br_fdb_cache)
		return -ENOMEM;

	get_random_bytes(&fdb_salt, sizeof(fdb_salt));
	return 0;
}

void br_fdb_fini(void)
{
	kmem_cache_destroy(br_fdb_cache);
}


/* if topology_changing then use forward_delay (default 15 sec)
 * otherwise keep longer (default 5 minutes)
 */
static inline unsigned long hold_time(const struct net_bridge *br)
{
	return br->topology_change ? br->forward_delay : br->ageing_time;
}

static inline int has_expired(const struct net_bridge *br,
				  const struct net_bridge_fdb_entry *fdb)
{
	return !fdb->is_static &&
		time_before_eq(fdb->updated + hold_time(br), jiffies);
}

static inline int br_mac_hash(const unsigned char *mac, __u16 vid)
{
	/* use 1 byte of OUI and 3 bytes of NIC */
	u32 key = get_unaligned((u32 *)(mac + 2));
	return jhash_2words(key, vid, fdb_salt) & (BR_HASH_SIZE - 1);
}

static void fdb_rcu_free(struct rcu_head *head)
{
	struct net_bridge_fdb_entry *ent
		= container_of(head, struct net_bridge_fdb_entry, rcu);
	kmem_cache_free(br_fdb_cache, ent);
#if defined(CONFIG_RTL_819X)
	rtl_fdb_delete_hooks(ent);
	fdb_entry_num--;
	if(fdb_entry_num < 0)
	{
		printk("fdb entry num error!!!!\n");
		fdb_entry_num = 0;
	}
#endif

}

/* When a static FDB entry is added, the mac address from the entry is
 * added to the bridge private HW address list and all required ports
 * are then updated with the new information.
 * Called under RTNL.
 */
static void fdb_add_hw_addr(struct net_bridge *br, const unsigned char *addr)
{
	int err;
	struct net_bridge_port *p;

	ASSERT_RTNL();

	list_for_each_entry(p, &br->port_list, list) {
		if (!br_promisc_port(p)) {
			err = dev_uc_add(p->dev, addr);
			if (err)
				goto undo;
		}
	}

	return;
undo:
	list_for_each_entry_continue_reverse(p, &br->port_list, list) {
		if (!br_promisc_port(p))
			dev_uc_del(p->dev, addr);
	}
}

/* When a static FDB entry is deleted, the HW address from that entry is
 * also removed from the bridge private HW address list and updates all
 * the ports with needed information.
 * Called under RTNL.
 */
static void fdb_del_hw_addr(struct net_bridge *br, const unsigned char *addr)
{
	struct net_bridge_port *p;

	ASSERT_RTNL();

	list_for_each_entry(p, &br->port_list, list) {
		if (!br_promisc_port(p))
			dev_uc_del(p->dev, addr);
	}
}

static void fdb_del_external_learn(struct net_bridge_fdb_entry *f)
{
	struct switchdev_obj_port_fdb fdb = {
		.obj = {
			.id = SWITCHDEV_OBJ_ID_PORT_FDB,
			.flags = SWITCHDEV_F_DEFER,
		},
		.vid = f->vlan_id,
	};

	ether_addr_copy(fdb.addr, f->addr.addr);
	switchdev_port_obj_del(f->dst->dev, &fdb.obj);
}

static void fdb_delete(struct net_bridge *br, struct net_bridge_fdb_entry *f)
{
	if (f->is_static)
		fdb_del_hw_addr(br, f->addr.addr);

	if (f->added_by_external_learn)
		fdb_del_external_learn(f);

	hlist_del_rcu(&f->hlist);
	fdb_notify(br, f, RTM_DELNEIGH);
	call_rcu(&f->rcu, fdb_rcu_free);
}

/* Delete a local entry if no other port had the same address. */
static void fdb_delete_local(struct net_bridge *br,
			     const struct net_bridge_port *p,
			     struct net_bridge_fdb_entry *f)
{
	const unsigned char *addr = f->addr.addr;
	struct net_bridge_vlan_group *vg;
	const struct net_bridge_vlan *v;
	struct net_bridge_port *op;
	u16 vid = f->vlan_id;

	/* Maybe another port has same hw addr? */
	list_for_each_entry(op, &br->port_list, list) {
		vg = nbp_vlan_group(op);
		if (op != p && ether_addr_equal(op->dev->dev_addr, addr) &&
		    (!vid || br_vlan_find(vg, vid))) {
			f->dst = op;
			f->added_by_user = 0;
			return;
		}
	}

	vg = br_vlan_group(br);
	v = br_vlan_find(vg, vid);
	/* Maybe bridge device has same hw addr? */
	if (p && ether_addr_equal(br->dev->dev_addr, addr) &&
	    (!vid || (v && br_vlan_should_use(v)))) {
		f->dst = NULL;
		f->added_by_user = 0;
		return;
	}

	fdb_delete(br, f);
}

void br_fdb_find_delete_local(struct net_bridge *br,
			      const struct net_bridge_port *p,
			      const unsigned char *addr, u16 vid)
{
	struct hlist_head *head = &br->hash[br_mac_hash(addr, vid)];
	struct net_bridge_fdb_entry *f;

	spin_lock_bh(&br->hash_lock);
	f = fdb_find(head, addr, vid);
	if (f && f->is_local && !f->added_by_user && f->dst == p)
		fdb_delete_local(br, p, f);
	spin_unlock_bh(&br->hash_lock);
}

void br_fdb_changeaddr(struct net_bridge_port *p, const unsigned char *newaddr)
{
	struct net_bridge_vlan_group *vg;
	struct net_bridge *br = p->br;
	struct net_bridge_vlan *v;
	int i;

	spin_lock_bh(&br->hash_lock);

	vg = nbp_vlan_group(p);
	/* Search all chains since old address/hash is unknown */
	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct hlist_node *h;
		hlist_for_each(h, &br->hash[i]) {
			struct net_bridge_fdb_entry *f;

			f = hlist_entry(h, struct net_bridge_fdb_entry, hlist);
			if (f->dst == p && f->is_local && !f->added_by_user) {
				/* delete old one */
				fdb_delete_local(br, p, f);

				/* if this port has no vlan information
				 * configured, we can safely be done at
				 * this point.
				 */
				if (!vg || !vg->num_vlans)
					goto insert;
			}
		}
	}

insert:
	/* insert new address,  may fail if invalid address or dup. */
	fdb_insert(br, p, newaddr, 0);

	if (!vg || !vg->num_vlans)
		goto done;

	/* Now add entries for every VLAN configured on the port.
	 * This function runs under RTNL so the bitmap will not change
	 * from under us.
	 */
	list_for_each_entry(v, &vg->vlan_list, vlist)
		fdb_insert(br, p, newaddr, v->vid);

done:
	spin_unlock_bh(&br->hash_lock);
}

void br_fdb_change_mac_address(struct net_bridge *br, const u8 *newaddr)
{
	struct net_bridge_vlan_group *vg;
	struct net_bridge_fdb_entry *f;
	struct net_bridge_vlan *v;

	spin_lock_bh(&br->hash_lock);

	/* If old entry was unassociated with any port, then delete it. */
	f = __br_fdb_get(br, br->dev->dev_addr, 0);
	if (f && f->is_local && !f->dst)
		fdb_delete_local(br, NULL, f);

	fdb_insert(br, NULL, newaddr, 0);
	vg = br_vlan_group(br);
	if (!vg || !vg->num_vlans)
		goto out;
	/* Now remove and add entries for every VLAN configured on the
	 * bridge.  This function runs under RTNL so the bitmap will not
	 * change from under us.
	 */
	list_for_each_entry(v, &vg->vlan_list, vlist) {
		if (!br_vlan_should_use(v))
			continue;
		f = __br_fdb_get(br, br->dev->dev_addr, v->vid);
		if (f && f->is_local && !f->dst)
			fdb_delete_local(br, NULL, f);
		fdb_insert(br, NULL, newaddr, v->vid);
	}
out:
	spin_unlock_bh(&br->hash_lock);
}
#if defined (CONFIG_RTL_IGMP_SNOOPING)
 void br_igmp_fdb_expired(struct net_bridge_fdb_entry *ent)
{
	int i2;
	unsigned long igmp_walktimeout;
	unsigned char *DA;
	unsigned char *SA;
	#if defined(MCAST_TO_UNICAST)
	struct net_device *dev=NULL;
	#endif

	igmp_walktimeout = jiffies - IGMP_EXPIRE_TIME;

	//IGMP_EXPIRE_TIME
	for (i2 = 0; i2 < FDB_IGMP_EXT_NUM; i2++)
	{
		if (ent->igmp_fdb_arr[i2].valid == 1) {

			// when timeout expire
			if (time_before_eq(ent->igmp_fdb_arr[i2].ageing_time, igmp_walktimeout))
			{
				DEBUG_PRINT("%s:%d\n", __FUNCTION__, __LINE__);
				SA = ent->igmp_fdb_arr[i2].SrcMac;
				DEBUG_PRINT("expired src mac:%02x,%02x,%02x,%02x,%02x,%02x\n",
					SA[0], SA[1], SA[2], SA[3], SA[4], SA[5]);

				DA = ent->addr.addr;
				DEBUG_PRINT("fdb:%02x:%02x:%02x-%02x:%02x:%02x\n",
					DA[0], DA[1], DA[2], DA[3], DA[4], DA[5]);



				/*---for process wlan client expired start---*/
#ifdef M2U_DELETE_CHECK
				if (rtl_M2UDeletecheck(DA, SA))
				{
#endif
				#if defined(MCAST_TO_UNICAST)
				dev = __dev_get_by_name(&init_net, RTL_PS_WLAN0_DEV_NAME);


				if (dev) {
					unsigned char StaMacAndGroup[20];

					memcpy(StaMacAndGroup, DA , 6);
					memcpy(StaMacAndGroup + 6, SA, 6);
				#if defined(CONFIG_COMPAT_NET_DEV_OPS)
					if (dev->do_ioctl != NULL) {
						dev->do_ioctl(dev, (struct ifreq*)StaMacAndGroup, 0x8B81);
				#else
					if (dev->netdev_ops->ndo_do_ioctl != NULL) {
						dev->netdev_ops->ndo_do_ioctl(dev, (struct ifreq*)StaMacAndGroup, 0x8B81);
				#endif
						DEBUG_PRINT("(fdb expire) wlan0 ioctl to DEL! M2U entry da:%02x:%02x:%02x-%02x:%02x:%02x; sa:%02x:%02x:%02x-%02x:%02x:%02x\n",
							StaMacAndGroup[0], StaMacAndGroup[1], StaMacAndGroup[2], StaMacAndGroup[3], StaMacAndGroup[4], StaMacAndGroup[5],
							StaMacAndGroup[6], StaMacAndGroup[7], StaMacAndGroup[8], StaMacAndGroup[9], StaMacAndGroup[10], StaMacAndGroup[11]);
					}
				}

				#endif /* MCAST_TO_UNICAST */
				/*---for process wlan client expired end---*/


				del_igmp_ext_entry(ent , SA , ent->igmp_fdb_arr[i2].port, 1);
#ifdef M2U_DELETE_CHECK
				}
#endif

				if (ent->portlist == 0)  // all joined sta are gone
				{
					DEBUG_PRINT("----all joined sta are gone,make it expired after %d seconds----\n", M2U_DELAY_DELETE_TIME);
					ent->updated = jiffies - (300 * HZ-M2U_DELAY_DELETE_TIME); // make it expired in 10s
				}

				if ((ent->portlist & 0x7f) == 0) {
					ent->group_src &=  ~(1 << 1); // eth0 all leave
				}

				if ((ent->portlist & 0x80) == 0) {
					ent->group_src &=  ~(1 << 2); // wlan0 all leave
				}


			}

		}

	}

}
#endif

void br_fdb_cleanup(unsigned long _data)
{
	struct net_bridge *br = (struct net_bridge *)_data;
	unsigned long delay = hold_time(br);
#ifdef CONFIG_RTL_819X
	unsigned long next_timer = jiffies + br->forward_delay;
#else
	unsigned long next_timer = jiffies + br->ageing_time;
#endif
	int i;

	spin_lock(&br->hash_lock);
	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct net_bridge_fdb_entry *f;
		struct hlist_node *n;

		hlist_for_each_entry_safe(f, n, &br->hash[i], hlist) {
			unsigned long this_timer;
#if defined(CONFIG_RTL_IGMP_SNOOPING)
			if (f->is_static &&
				f->igmpFlag &&
				(MULTICAST_MAC(f->addr.addr)
#if defined(CONFIG_RTL_MLD_SNOOPING)
				|| IPV6_MULTICAST_MAC(f->addr.addr)
#endif
				))
			{
				br_igmp_fdb_expired(f);

				if (time_before_eq(f->updated + 300 * HZ, jiffies))
				{
					DEBUG_PRINT("fdb_delete:f->addr.addr is 0x%02x:%02x:%02x-%02x:%02x:%02x\n",
					f->addr.addr[0], f->addr.addr[1], f->addr.addr[2], f->addr.addr[3], f->addr.addr[4], f->addr.addr[5]);
					fdb_delete(br, f);
				}

			}
#endif

			if (f->is_static)
				continue;
			if (f->added_by_external_learn)
				continue;

			#if defined(CONFIG_RTL_819X)
			rtl_br_fdb_cleanup_hooks(br,f, delay);
			#endif

			this_timer = f->updated + delay;
			if (time_before_eq(this_timer, jiffies))
				fdb_delete(br, f);
			else if (time_before(this_timer, next_timer))
				next_timer = this_timer;
		}
	}
	spin_unlock(&br->hash_lock);

	mod_timer(&br->gc_timer, round_jiffies_up(next_timer));
}

/* Completely flush all dynamic entries in forwarding database.*/
void br_fdb_flush(struct net_bridge *br)
{
	int i;

	spin_lock_bh(&br->hash_lock);
	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct net_bridge_fdb_entry *f;
		struct hlist_node *n;
		hlist_for_each_entry_safe(f, n, &br->hash[i], hlist) {
#if defined(CONFIG_RTL_IGMP_SNOOPING)
			if (f->is_static &&
				f->igmpFlag &&
				MULTICAST_MAC(f->addr.addr))
			{
				br_igmp_fdb_expired(f);
				if (time_before_eq(f->updated + 300 * HZ, jiffies))
				{
					fdb_delete(br,f);
				}
			}
#endif
			if (!f->is_static)
				fdb_delete(br, f);
		}
	}
	spin_unlock_bh(&br->hash_lock);
}

/* Flush all entries referring to a specific port.
 * if do_all is set also flush static entries
 * if vid is set delete all entries that match the vlan_id
 */
void br_fdb_delete_by_port(struct net_bridge *br,
			   const struct net_bridge_port *p,
			   u16 vid,
			   int do_all)
{
	int i;

	spin_lock_bh(&br->hash_lock);
	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct hlist_node *h, *g;

		hlist_for_each_safe(h, g, &br->hash[i]) {
			struct net_bridge_fdb_entry *f
				= hlist_entry(h, struct net_bridge_fdb_entry, hlist);
			if (f->dst != p)
				continue;

			if (!do_all)
				if (f->is_static || (vid && f->vlan_id != vid))
					continue;

			if (f->is_local)
				fdb_delete_local(br, p, f);
			else
				fdb_delete(br, f);
		}
	}
	spin_unlock_bh(&br->hash_lock);
}

/* No locking or refcounting, assumes caller has rcu_read_lock */
struct net_bridge_fdb_entry *__br_fdb_get(struct net_bridge *br,
					  const unsigned char *addr,
					  __u16 vid)
{
	struct net_bridge_fdb_entry *fdb;

	hlist_for_each_entry_rcu(fdb,
				&br->hash[br_mac_hash(addr, vid)], hlist) {
		if (ether_addr_equal(fdb->addr.addr, addr) &&
		    fdb->vlan_id == vid) {
			if (unlikely(has_expired(br, fdb))) {
				#if defined(CONFIG_RTL_819X)
				if (rtl___br_fdb_get_timeout_hooks(br, fdb, addr) == RTL_PS_HOOKS_BREAK) {
					break;
				}
				#else
				break;
				#endif
			}
			return fdb;
		}
	}

	return NULL;
}

#if IS_ENABLED(CONFIG_ATM_LANE)
/* Interface used by ATM LANE hook to test
 * if an addr is on some other bridge port */
int br_fdb_test_addr(struct net_device *dev, unsigned char *addr)
{
	struct net_bridge_fdb_entry *fdb;
	struct net_bridge_port *port;
	int ret;

	rcu_read_lock();
	port = br_port_get_rcu(dev);
	if (!port)
		ret = 0;
	else {
		fdb = __br_fdb_get(port->br, addr, 0);
		ret = fdb && fdb->dst && fdb->dst->dev != dev &&
			fdb->dst->state == BR_STATE_FORWARDING;
	}
	rcu_read_unlock();

	return ret;
}
#endif /* CONFIG_ATM_LANE */

/*
 * Fill buffer with forwarding table records in
 * the API format.
 */
int br_fdb_fillbuf(struct net_bridge *br, void *buf,
		   unsigned long maxnum, unsigned long skip)
{
	struct __fdb_entry *fe = buf;
	int i, num = 0;
	struct net_bridge_fdb_entry *f;

	memset(buf, 0, maxnum*sizeof(struct __fdb_entry));

	rcu_read_lock();
	for (i = 0; i < BR_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(f, &br->hash[i], hlist) {
			if (num >= maxnum)
				goto out;

			if (has_expired(br, f))
				continue;

			/* ignore pseudo entry for local MAC address */
			if (!f->dst)
				continue;

			if (skip) {
				--skip;
				continue;
			}

			/* convert from internal format to API */
			memcpy(fe->mac_addr, f->addr.addr, ETH_ALEN);

			/* due to ABI compat need to split into hi/lo */
			fe->port_no = f->dst->port_no;
			fe->port_hi = f->dst->port_no >> 8;

			fe->is_local = f->is_local;
			if (!f->is_static)
				fe->ageing_timer_value = jiffies_delta_to_clock_t(jiffies - f->updated);
			++fe;
			++num;
		}
	}

 out:
	rcu_read_unlock();

	return num;
}

static struct net_bridge_fdb_entry *fdb_find(struct hlist_head *head,
					     const unsigned char *addr,
					     __u16 vid)
{
	struct net_bridge_fdb_entry *fdb;

	hlist_for_each_entry(fdb, head, hlist) {
		if (ether_addr_equal(fdb->addr.addr, addr) &&
		    fdb->vlan_id == vid)
			return fdb;
	}
	return NULL;
}

static struct net_bridge_fdb_entry *fdb_find_rcu(struct hlist_head *head,
						 const unsigned char *addr,
						 __u16 vid)
{
	struct net_bridge_fdb_entry *fdb;

	hlist_for_each_entry_rcu(fdb, head, hlist) {
		if (ether_addr_equal(fdb->addr.addr, addr) &&
		    fdb->vlan_id == vid)
			return fdb;
	}
	return NULL;
}

static struct net_bridge_fdb_entry *fdb_create(struct hlist_head *head,
					       struct net_bridge_port *source,
					       const unsigned char *addr,
					       __u16 vid,
					       unsigned char is_local,
					       unsigned char is_static)
{
	struct net_bridge_fdb_entry *fdb;
#if defined(CONFIG_RTL_IGMP_SNOOPING)
	int i3;
#endif
#if defined(CONFIG_RTL_819X)
	if (fdb_entry_num >= fdb_entry_max)
		return NULL;
#endif
	fdb = kmem_cache_alloc(br_fdb_cache, GFP_ATOMIC);
	if (fdb) {
		memcpy(fdb->addr.addr, addr, ETH_ALEN);
		#if defined(CONFIG_RTL_819X)
		fdb_entry_num++;
		#endif
		fdb->dst = source;
		fdb->vlan_id = vid;
		fdb->is_local = is_local;
		fdb->is_static = is_static;
		fdb->added_by_user = 0;
		fdb->added_by_external_learn = 0;
		fdb->updated = fdb->used = jiffies;
#if defined (CONFIG_RTL_IGMP_SNOOPING)
		fdb->group_src = 0;
		fdb->igmpFlag=0;
		for(i3=0 ; i3<FDB_IGMP_EXT_NUM ;i3++)
		{
			fdb->igmp_fdb_arr[i3].valid = 0;
			fdb->portUsedNum[i3] = 0;
		}
#endif
		#if defined(CONFIG_RTL_819X)
		rtl_fdb_create_hooks(fdb, addr);
		#endif
		hlist_add_head_rcu(&fdb->hlist, head);
	}
	return fdb;
}

static int fdb_insert(struct net_bridge *br, struct net_bridge_port *source,
		  const unsigned char *addr, u16 vid)
{
	struct hlist_head *head = &br->hash[br_mac_hash(addr, vid)];
	struct net_bridge_fdb_entry *fdb;
#if defined(CONFIG_RTL_IGMP_SNOOPING)
	if (((addr[0] == 0xff) && (addr[1] == 0xff) && (addr[2] == 0xff) && (addr[3] == 0xff) && (addr[4] == 0xff) && (addr[5] == 0xff)) ||
		((addr[0] == 0) && (addr[1] == 0) && (addr[2] == 0) && (addr[3] == 0) && (addr[4] == 0) && (addr[5] == 0)))
	{
		return -EINVAL;
	}
#else
	if (!is_valid_ether_addr(addr))
		return -EINVAL;
#endif
	fdb = fdb_find(head, addr, vid);
	if (fdb) {
		/* it is okay to have multiple ports with same
		 * address, just use the first one.
		 */
		if (fdb->is_local)
			return 0;
		br_warn(br, "adding interface %s with same address "
		       "as a received packet\n",
		       source ? source->dev->name : br->dev->name);
		fdb_delete(br, fdb);
	}

	fdb = fdb_create(head, source, addr, vid, 1, 1);
	if (!fdb)
		return -ENOMEM;

	fdb_add_hw_addr(br, addr);
	fdb_notify(br, fdb, RTM_NEWNEIGH);
	return 0;
}

/* Add entry for local address of interface */
int br_fdb_insert(struct net_bridge *br, struct net_bridge_port *source,
		  const unsigned char *addr, u16 vid)
{
	int ret;

	spin_lock_bh(&br->hash_lock);
	ret = fdb_insert(br, source, addr, vid);
	spin_unlock_bh(&br->hash_lock);
	return ret;
}

void br_fdb_update(struct net_bridge *br, struct net_bridge_port *source,
		   const unsigned char *addr, u16 vid, bool added_by_user)
{
	struct hlist_head *head = &br->hash[br_mac_hash(addr, vid)];
	struct net_bridge_fdb_entry *fdb;
	bool fdb_modified = false;

#if defined(CONFIG_RTL_819X)
	struct ifreq frq;
	extern void update_hw_l2table(const char *srcName,const unsigned char *addr);
#endif /* CONFIG_RTL_819X */

	/* some users want to always flood. */
	if (hold_time(br) == 0)
		return;

	/* ignore packets unless we are using this port */
	if (!(source->state == BR_STATE_LEARNING ||
	      source->state == BR_STATE_FORWARDING))
		return;

	fdb = fdb_find_rcu(head, addr, vid);
	if (likely(fdb)) {
		/* attempt to update an entry for a local interface */
		if (unlikely(fdb->is_local)) {
			if (net_ratelimit())
				br_warn(br, "received packet on %s with "
					"own address as source address\n",
					source->dev->name);
		} else {
			/* fastpath: update of existing entry */
			if (unlikely(source != fdb->dst)) {
#if defined(CONFIG_RTL_819X)
				if (!memcmp(fdb->dst->dev->name, "eth", 3)) {
					memset(&frq, 0, sizeof(struct ifreq));
					if (fdb->dst->dev->netdev_ops->ndo_do_ioctl != NULL)
						fdb->dst->dev->netdev_ops->ndo_do_ioctl(fdb->dst->dev, &frq, 2210);
					update_hw_l2table("wlan", fdb->addr.addr); /* RTL_WLAN_NAME */
				}
#endif /* CONFIG_RTL_819X */
				fdb->dst = source;
				fdb_modified = true;
			}
			fdb->updated = jiffies;
			if (unlikely(added_by_user))
				fdb->added_by_user = 1;
			if (unlikely(fdb_modified))
				fdb_notify(br, fdb, RTM_NEWNEIGH);

			#if defined(CONFIG_RTL_819X) && defined(CONFIG_RTL_EXT_PORT_SUPPORT)
			rtl_fdb_create_hooks(fdb, addr);
			#endif
		}
	} else {
		spin_lock(&br->hash_lock);
		if (likely(!fdb_find(head, addr, vid))) {
			fdb = fdb_create(head, source, addr, vid, 0, 0);
			if (fdb) {
				if (unlikely(added_by_user))
					fdb->added_by_user = 1;
				fdb_notify(br, fdb, RTM_NEWNEIGH);
			}
		}
		/* else  we lose race and someone else inserts
		 * it first, don't bother updating
		 */
		spin_unlock(&br->hash_lock);
	}
}

static int fdb_to_nud(const struct net_bridge *br,
		      const struct net_bridge_fdb_entry *fdb)
{
	if (fdb->is_local)
		return NUD_PERMANENT;
	else if (fdb->is_static)
		return NUD_NOARP;
	else if (has_expired(br, fdb))
		return NUD_STALE;
	else
		return NUD_REACHABLE;
}

static int fdb_fill_info(struct sk_buff *skb, const struct net_bridge *br,
			 const struct net_bridge_fdb_entry *fdb,
			 u32 portid, u32 seq, int type, unsigned int flags)
{
	unsigned long now = jiffies;
	struct nda_cacheinfo ci;
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;

	nlh = nlmsg_put(skb, portid, seq, type, sizeof(*ndm), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	ndm = nlmsg_data(nlh);
	ndm->ndm_family	 = AF_BRIDGE;
	ndm->ndm_pad1    = 0;
	ndm->ndm_pad2    = 0;
	ndm->ndm_flags	 = fdb->added_by_external_learn ? NTF_EXT_LEARNED : 0;
	ndm->ndm_type	 = 0;
	ndm->ndm_ifindex = fdb->dst ? fdb->dst->dev->ifindex : br->dev->ifindex;
	ndm->ndm_state   = fdb_to_nud(br, fdb);

	if (nla_put(skb, NDA_LLADDR, ETH_ALEN, &fdb->addr))
		goto nla_put_failure;
	if (nla_put_u32(skb, NDA_MASTER, br->dev->ifindex))
		goto nla_put_failure;
	ci.ndm_used	 = jiffies_to_clock_t(now - fdb->used);
	ci.ndm_confirmed = 0;
	ci.ndm_updated	 = jiffies_to_clock_t(now - fdb->updated);
	ci.ndm_refcnt	 = 0;
	if (nla_put(skb, NDA_CACHEINFO, sizeof(ci), &ci))
		goto nla_put_failure;

	if (fdb->vlan_id && nla_put(skb, NDA_VLAN, sizeof(u16), &fdb->vlan_id))
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static inline size_t fdb_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct ndmsg))
		+ nla_total_size(ETH_ALEN) /* NDA_LLADDR */
		+ nla_total_size(sizeof(u32)) /* NDA_MASTER */
		+ nla_total_size(sizeof(u16)) /* NDA_VLAN */
		+ nla_total_size(sizeof(struct nda_cacheinfo));
}

static void fdb_notify(struct net_bridge *br,
		       const struct net_bridge_fdb_entry *fdb, int type)
{
	struct net *net = dev_net(br->dev);
	struct sk_buff *skb;
	int err = -ENOBUFS;

	skb = nlmsg_new(fdb_nlmsg_size(), GFP_ATOMIC);
	if (skb == NULL)
		goto errout;

	err = fdb_fill_info(skb, br, fdb, 0, 0, type, 0);
	if (err < 0) {
		/* -EMSGSIZE implies BUG in fdb_nlmsg_size() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}
	rtnl_notify(skb, net, 0, RTNLGRP_NEIGH, NULL, GFP_ATOMIC);
	return;
errout:
	rtnl_set_sk_err(net, RTNLGRP_NEIGH, err);
}

/* Dump information about entries, in response to GETNEIGH */
int br_fdb_dump(struct sk_buff *skb,
		struct netlink_callback *cb,
		struct net_device *dev,
		struct net_device *filter_dev,
		int idx)
{
	struct net_bridge *br = netdev_priv(dev);
	int i;

	if (!(dev->priv_flags & IFF_EBRIDGE))
		goto out;

	if (!filter_dev)
		idx = ndo_dflt_fdb_dump(skb, cb, dev, NULL, idx);

	for (i = 0; i < BR_HASH_SIZE; i++) {
		struct net_bridge_fdb_entry *f;

		hlist_for_each_entry_rcu(f, &br->hash[i], hlist) {
			if (idx < cb->args[0])
				goto skip;

			if (filter_dev &&
			    (!f->dst || f->dst->dev != filter_dev)) {
				if (filter_dev != dev)
					goto skip;
				/* !f->dst is a special case for bridge
				 * It means the MAC belongs to the bridge
				 * Therefore need a little more filtering
				 * we only want to dump the !f->dst case
				 */
				if (f->dst)
					goto skip;
			}
			if (!filter_dev && f->dst)
				goto skip;

			if (fdb_fill_info(skb, br, f,
					  NETLINK_CB(cb->skb).portid,
					  cb->nlh->nlmsg_seq,
					  RTM_NEWNEIGH,
					  NLM_F_MULTI) < 0)
				break;
skip:
			++idx;
		}
	}

out:
	return idx;
}

/* Update (create or replace) forwarding database entry */
static int fdb_add_entry(struct net_bridge_port *source, const __u8 *addr,
			 __u16 state, __u16 flags, __u16 vid)
{
	struct net_bridge *br = source->br;
	struct hlist_head *head = &br->hash[br_mac_hash(addr, vid)];
	struct net_bridge_fdb_entry *fdb;
	bool modified = false;

	/* If the port cannot learn allow only local and static entries */
	if (!(state & NUD_PERMANENT) && !(state & NUD_NOARP) &&
	    !(source->state == BR_STATE_LEARNING ||
	      source->state == BR_STATE_FORWARDING))
		return -EPERM;

	fdb = fdb_find(head, addr, vid);
	if (fdb == NULL) {
		if (!(flags & NLM_F_CREATE))
			return -ENOENT;

		fdb = fdb_create(head, source, addr, vid, 0, 0);
		if (!fdb)
			return -ENOMEM;

		modified = true;
	} else {
		if (flags & NLM_F_EXCL)
			return -EEXIST;

		if (fdb->dst != source) {
			fdb->dst = source;
			modified = true;
		}
	}

	if (fdb_to_nud(br, fdb) != state) {
		if (state & NUD_PERMANENT) {
			fdb->is_local = 1;
			if (!fdb->is_static) {
				fdb->is_static = 1;
				fdb_add_hw_addr(br, addr);
			}
		} else if (state & NUD_NOARP) {
			fdb->is_local = 0;
			if (!fdb->is_static) {
				fdb->is_static = 1;
				fdb_add_hw_addr(br, addr);
			}
		} else {
			fdb->is_local = 0;
			if (fdb->is_static) {
				fdb->is_static = 0;
				fdb_del_hw_addr(br, addr);
			}
		}

		modified = true;
	}
	fdb->added_by_user = 1;

	fdb->used = jiffies;
	if (modified) {
		fdb->updated = jiffies;
		fdb_notify(br, fdb, RTM_NEWNEIGH);
	}

	return 0;
}

static int __br_fdb_add(struct ndmsg *ndm, struct net_bridge_port *p,
	       const unsigned char *addr, u16 nlh_flags, u16 vid)
{
	int err = 0;

	if (ndm->ndm_flags & NTF_USE) {
		local_bh_disable();
		rcu_read_lock();
		br_fdb_update(p->br, p, addr, vid, true);
		rcu_read_unlock();
		local_bh_enable();
	} else {
		spin_lock_bh(&p->br->hash_lock);
		err = fdb_add_entry(p, addr, ndm->ndm_state,
				    nlh_flags, vid);
		spin_unlock_bh(&p->br->hash_lock);
	}

	return err;
}

/* Add new permanent fdb entry with RTM_NEWNEIGH */
int br_fdb_add(struct ndmsg *ndm, struct nlattr *tb[],
	       struct net_device *dev,
	       const unsigned char *addr, u16 vid, u16 nlh_flags)
{
	struct net_bridge_vlan_group *vg;
	struct net_bridge_port *p = NULL;
	struct net_bridge_vlan *v;
	struct net_bridge *br = NULL;
	int err = 0;

	if (!(ndm->ndm_state & (NUD_PERMANENT|NUD_NOARP|NUD_REACHABLE))) {
		pr_info("bridge: RTM_NEWNEIGH with invalid state %#x\n", ndm->ndm_state);
		return -EINVAL;
	}

	if (is_zero_ether_addr(addr)) {
		pr_info("bridge: RTM_NEWNEIGH with invalid ether address\n");
		return -EINVAL;
	}

	if (dev->priv_flags & IFF_EBRIDGE) {
		br = netdev_priv(dev);
		vg = br_vlan_group(br);
	} else {
		p = br_port_get_rtnl(dev);
		if (!p) {
			pr_info("bridge: RTM_NEWNEIGH %s not a bridge port\n",
				dev->name);
			return -EINVAL;
		}
		vg = nbp_vlan_group(p);
	}

	if (vid) {
		v = br_vlan_find(vg, vid);
		if (!v || !br_vlan_should_use(v)) {
			pr_info("bridge: RTM_NEWNEIGH with unconfigured vlan %d on %s\n", vid, dev->name);
			return -EINVAL;
		}

		/* VID was specified, so use it. */
		if (dev->priv_flags & IFF_EBRIDGE)
			err = br_fdb_insert(br, NULL, addr, vid);
		else
			err = __br_fdb_add(ndm, p, addr, nlh_flags, vid);
	} else {
		if (dev->priv_flags & IFF_EBRIDGE)
			err = br_fdb_insert(br, NULL, addr, 0);
		else
			err = __br_fdb_add(ndm, p, addr, nlh_flags, 0);
		if (err || !vg || !vg->num_vlans)
			goto out;

		/* We have vlans configured on this port and user didn't
		 * specify a VLAN.  To be nice, add/update entry for every
		 * vlan on this port.
		 */
		list_for_each_entry(v, &vg->vlan_list, vlist) {
			if (!br_vlan_should_use(v))
				continue;
			if (dev->priv_flags & IFF_EBRIDGE)
				err = br_fdb_insert(br, NULL, addr, v->vid);
			else
				err = __br_fdb_add(ndm, p, addr, nlh_flags,
						   v->vid);
			if (err)
				goto out;
		}
	}

out:
	return err;
}

static int fdb_delete_by_addr(struct net_bridge *br, const u8 *addr,
			      u16 vid)
{
	struct hlist_head *head = &br->hash[br_mac_hash(addr, vid)];
	struct net_bridge_fdb_entry *fdb;

	fdb = fdb_find(head, addr, vid);
	if (!fdb)
		return -ENOENT;

	fdb_delete(br, fdb);
	return 0;
}

static int __br_fdb_delete_by_addr(struct net_bridge *br,
				   const unsigned char *addr, u16 vid)
{
	int err;

	spin_lock_bh(&br->hash_lock);
	err = fdb_delete_by_addr(br, addr, vid);
	spin_unlock_bh(&br->hash_lock);

	return err;
}

static int fdb_delete_by_addr_and_port(struct net_bridge_port *p,
				       const u8 *addr, u16 vlan)
{
	struct net_bridge *br = p->br;
	struct hlist_head *head = &br->hash[br_mac_hash(addr, vlan)];
	struct net_bridge_fdb_entry *fdb;

	fdb = fdb_find(head, addr, vlan);
	if (!fdb || fdb->dst != p)
		return -ENOENT;

	fdb_delete(br, fdb);
	return 0;
}

static int __br_fdb_delete(struct net_bridge_port *p,
			   const unsigned char *addr, u16 vid)
{
	int err;

	spin_lock_bh(&p->br->hash_lock);
	err = fdb_delete_by_addr_and_port(p, addr, vid);
	spin_unlock_bh(&p->br->hash_lock);

	return err;
}

/* Remove neighbor entry with RTM_DELNEIGH */
int br_fdb_delete(struct ndmsg *ndm, struct nlattr *tb[],
		  struct net_device *dev,
		  const unsigned char *addr, u16 vid)
{
	struct net_bridge_vlan_group *vg;
	struct net_bridge_port *p = NULL;
	struct net_bridge_vlan *v;
	struct net_bridge *br = NULL;
	int err;

	if (dev->priv_flags & IFF_EBRIDGE) {
		br = netdev_priv(dev);
		vg = br_vlan_group(br);
	} else {
		p = br_port_get_rtnl(dev);
		if (!p) {
			pr_info("bridge: RTM_DELNEIGH %s not a bridge port\n",
				dev->name);
			return -EINVAL;
		}
		vg = nbp_vlan_group(p);
	}

	if (vid) {
		v = br_vlan_find(vg, vid);
		if (!v) {
			pr_info("bridge: RTM_DELNEIGH with unconfigured vlan %d on %s\n", vid, dev->name);
			return -EINVAL;
		}

		if (dev->priv_flags & IFF_EBRIDGE)
			err = __br_fdb_delete_by_addr(br, addr, vid);
		else
			err = __br_fdb_delete(p, addr, vid);
	} else {
		err = -ENOENT;
		if (dev->priv_flags & IFF_EBRIDGE)
			err = __br_fdb_delete_by_addr(br, addr, 0);
		else
			err &= __br_fdb_delete(p, addr, 0);

		if (!vg || !vg->num_vlans)
			goto out;

		list_for_each_entry(v, &vg->vlan_list, vlist) {
			if (!br_vlan_should_use(v))
				continue;
			if (dev->priv_flags & IFF_EBRIDGE)
				err = __br_fdb_delete_by_addr(br, addr, v->vid);
			else
				err &= __br_fdb_delete(p, addr, v->vid);
		}
	}
out:
	return err;
}

int br_fdb_sync_static(struct net_bridge *br, struct net_bridge_port *p)
{
	struct net_bridge_fdb_entry *fdb, *tmp;
	int i;
	int err;

	ASSERT_RTNL();

	for (i = 0; i < BR_HASH_SIZE; i++) {
		hlist_for_each_entry(fdb, &br->hash[i], hlist) {
			/* We only care for static entries */
			if (!fdb->is_static)
				continue;

			err = dev_uc_add(p->dev, fdb->addr.addr);
			if (err)
				goto rollback;
		}
	}
	return 0;

rollback:
	for (i = 0; i < BR_HASH_SIZE; i++) {
		hlist_for_each_entry(tmp, &br->hash[i], hlist) {
			/* If we reached the fdb that failed, we can stop */
			if (tmp == fdb)
				break;

			/* We only care for static entries */
			if (!tmp->is_static)
				continue;

			dev_uc_del(p->dev, tmp->addr.addr);
		}
	}
	return err;
}

void br_fdb_unsync_static(struct net_bridge *br, struct net_bridge_port *p)
{
	struct net_bridge_fdb_entry *fdb;
	int i;

	ASSERT_RTNL();

	for (i = 0; i < BR_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(fdb, &br->hash[i], hlist) {
			/* We only care for static entries */
			if (!fdb->is_static)
				continue;

			dev_uc_del(p->dev, fdb->addr.addr);
		}
	}
}

int br_fdb_external_learn_add(struct net_bridge *br, struct net_bridge_port *p,
			      const unsigned char *addr, u16 vid)
{
	struct hlist_head *head;
	struct net_bridge_fdb_entry *fdb;
	int err = 0;

	ASSERT_RTNL();
	spin_lock_bh(&br->hash_lock);

	head = &br->hash[br_mac_hash(addr, vid)];
	fdb = fdb_find(head, addr, vid);
	if (!fdb) {
		fdb = fdb_create(head, p, addr, vid, 0, 0);
		if (!fdb) {
			err = -ENOMEM;
			goto err_unlock;
		}
		fdb->added_by_external_learn = 1;
		fdb_notify(br, fdb, RTM_NEWNEIGH);
	} else if (fdb->added_by_external_learn) {
		/* Refresh entry */
		fdb->updated = fdb->used = jiffies;
	} else if (!fdb->added_by_user) {
		/* Take over SW learned entry */
		fdb->added_by_external_learn = 1;
		fdb->updated = jiffies;
		fdb_notify(br, fdb, RTM_NEWNEIGH);
	}

err_unlock:
	spin_unlock_bh(&br->hash_lock);

	return err;
}

int br_fdb_external_learn_del(struct net_bridge *br, struct net_bridge_port *p,
			      const unsigned char *addr, u16 vid)
{
	struct hlist_head *head;
	struct net_bridge_fdb_entry *fdb;
	int err = 0;

	ASSERT_RTNL();
	spin_lock_bh(&br->hash_lock);

	head = &br->hash[br_mac_hash(addr, vid)];
	fdb = fdb_find(head, addr, vid);
	if (fdb && fdb->added_by_external_learn)
		fdb_delete(br, fdb);
	else
		err = -ENOENT;

	spin_unlock_bh(&br->hash_lock);

	return err;
}

#if defined(CONFIG_RTL_IGMP_SNOOPING) || defined(CONFIG_BRIDGE_IGMP_SNOOPING)
int chk_igmp_ext_entry(
	struct net_bridge_fdb_entry *fdb ,
	unsigned char *srcMac)
{

	int i2;
	unsigned char *add;
	add = fdb->addr.addr;

	for (i2 = 0 ; i2 < FDB_IGMP_EXT_NUM ; i2++) {
		if(fdb->igmp_fdb_arr[i2].valid == 1) {
			if(!memcmp(fdb->igmp_fdb_arr[i2].SrcMac, srcMac, 6)){
				return 1;
			}
		}
	}
	return 0;
}
#ifdef CONFIG_RTL_IGMP_SNOOPING
extern int bitmask_to_id(unsigned char val);
#else
static int bitmask_to_id(unsigned char val)
{
	int i;
	for (i = 0; i < 8; i++) {
		if (val & (1 << i))
			break;
	}

	if (i >= 8)
	{
		i = 7;
	}
	return (i);
}
#endif
void add_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,
	unsigned char *srcMac , unsigned char portComeIn)
{
	int i2;
	unsigned char *add;
	add = fdb->addr.addr;

	DEBUG_PRINT("add_igmp,DA=%02x:%02x:%02x:%02x:%02x:%02x ; SA=%02x:%02x:%02x:%02x:%02x:%02x\n",
		add[0], add[1], add[2], add[3], add[4], add[5],
		srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);

	for (i2 = 0; i2 < FDB_IGMP_EXT_NUM; i2++) {
		if (fdb->igmp_fdb_arr[i2].valid == 0) {
			fdb->igmp_fdb_arr[i2].valid = 1;
			fdb->igmp_fdb_arr[i2].ageing_time = jiffies;
			memcpy(fdb->igmp_fdb_arr[i2].SrcMac, srcMac, 6);
			fdb->igmp_fdb_arr[i2].port = portComeIn ;
			fdb->portlist |= portComeIn;
			fdb->portUsedNum[bitmask_to_id(portComeIn)]++;
			DEBUG_PRINT("portUsedNum[%d]=%d\n\n", bitmask_to_id(portComeIn), fdb->portUsedNum[bitmask_to_id(portComeIn)]);
			return;
		}
	}
	DEBUG_PRINT("%s:entry Rdy existed!!!\n", __FUNCTION__);
}

void update_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,
	unsigned char *srcMac , unsigned char portComeIn)
{
	int i2;
	unsigned char *add;
	add = fdb->addr.addr;

		DEBUG_PRINT("update_igmp,DA=%02x:%02x:%02x:%02x:%02x:%02x ; SA=%02x:%02x:%02x:%02x:%02x:%02x\n",
		add[0], add[1], add[2], add[3], add[4], add[5],
		srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);

	for (i2 = 0; i2 < FDB_IGMP_EXT_NUM; i2++) {
		if (fdb->igmp_fdb_arr[i2].valid == 1) {
			if (!memcmp(fdb->igmp_fdb_arr[i2].SrcMac, srcMac, 6)) {

				fdb->igmp_fdb_arr[i2].ageing_time = jiffies;
				if (fdb->igmp_fdb_arr[i2].port != portComeIn) {

					unsigned char port_orig = fdb->igmp_fdb_arr[i2].port;
					int index = bitmask_to_id(port_orig);

					fdb->portUsedNum[index]--;
					DEBUG_PRINT("(--) portUsedNum[%d]=%d\n", index , fdb->portUsedNum[index]);
					if (fdb->portUsedNum[index] <= 0) {
						fdb->portlist &= ~(port_orig);
						if (fdb->portUsedNum[index] < 0) {
							DEBUG_PRINT("!! portNum[%d] < 0 at (update_igmp_ext_entry)\n", index);
							fdb->portUsedNum[index] = 0;
						}
					}


					fdb->portUsedNum[bitmask_to_id(portComeIn)]++;
					DEBUG_PRINT("(++) portUsedNum[%d]=%d\n", bitmask_to_id(portComeIn), fdb->portUsedNum[bitmask_to_id(portComeIn)]);
					fdb->portlist |= portComeIn;


					fdb->igmp_fdb_arr[i2].port = portComeIn;
					DEBUG_PRINT("	!!! portlist be updated:%x !!!!\n", fdb->portlist);

				}
				return;
			}
		}
	}

	DEBUG_PRINT("%s: ...fail!!\n", __FUNCTION__);
}


void del_igmp_ext_entry(struct net_bridge_fdb_entry *fdb ,unsigned char *srcMac , unsigned char portComeIn,unsigned char expireFlag )
{
	int i2;
	unsigned char *add;
	add = fdb->addr.addr;

	for (i2 = 0; i2 < FDB_IGMP_EXT_NUM; i2++) {
		if (fdb->igmp_fdb_arr[i2].valid == 1) {
			if (!memcmp(fdb->igmp_fdb_arr[i2].SrcMac, srcMac, 6))
			{
				if (expireFlag)
				{
					fdb->igmp_fdb_arr[i2].ageing_time -=  300 * HZ; // make it expired
					fdb->igmp_fdb_arr[i2].valid = 0;

					DEBUG_PRINT("\ndel_igmp_ext_entry,DA=%02x:%02x:%02x:%02x:%02x:%02x ; SA=%02x:%02x:%02x:%02x:%02x:%02x success!!!\n",
					add[0], add[1], add[2], add[3], add[4], add[5],
					srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);

					//DEBUG_PRINT("%s:success!!\n", __FUNCTION__);

					if (portComeIn != 0) {
						int index;
						index = bitmask_to_id(portComeIn);
						fdb->portUsedNum[index]--;
						if (fdb->portUsedNum[index] <= 0) {
							DEBUG_PRINT("portUsedNum[%d] == 0 ,update portlist from (%x)  " , index, fdb->portlist);
							fdb->portlist &= ~ portComeIn;
							DEBUG_PRINT("to (%x) \n" , fdb->portlist);

							if (fdb->portUsedNum[index] < 0) {
							DEBUG_PRINT("!! portUsedNum[%d]=%d < 0 at (del_igmp_ext_entry)  \n" , index, fdb->portUsedNum[index]);
							fdb->portUsedNum[index] = 0;
							}
						}else{
							DEBUG_PRINT("(del) portUsedNum[%d] = %d \n" , index, fdb->portUsedNum[index]);
						}

					}
					DEBUG_PRINT("\n");
				}
				else
				{
					fdb->igmp_fdb_arr[i2].ageing_time = jiffies - (IGMP_EXPIRE_TIME - M2U_DELAY_DELETE_TIME); //delay 10s to expire
					DEBUG_PRINT("\nexpireFlag:%d, delay %d sec to del_igmp_ext_entry,DA=%02x:%02x:%02x:%02x:%02x:%02x ; SA=%02x:%02x:%02x:%02x:%02x:%02x\n",
						expireFlag, M2U_DELAY_DELETE_TIME / HZ,
						add[0], add[1], add[2], add[3], add[4], add[5],
						srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);
				}
				return ;
			}
		}
	}

	DEBUG_PRINT("%s:entry not existed!!\n\n", __FUNCTION__);
}


#endif
