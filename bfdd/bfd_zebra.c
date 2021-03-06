/*
 * BFDD - bfd_zebra.c   
 *
 * Copyright (C) 2007   Jaroslaw Adam Gralak
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public Licenseas published by the Free 
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.

 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */


#include <zebra.h>

#include "command.h"
#include "stream.h"
#include "network.h"
#include "prefix.h"
#include "log.h"
#include "sockunion.h"
#include "zclient.h"
#include "routemap.h"
#include "thread.h"
#include "hash.h"
#include "table.h"

#include "bfd.h"
#include "bfdd/bfdd.h"
#include "bfdd/bfd_zebra.h"
#include "bfdd/bfd_debug.h"
#include "bfdd/bfd_interface.h"
#include "bfdd/bfd_fsm.h"
#include "bfdd/bfd_packet.h"

#include "zebra/zserv.h"
#include "zebra/zserv_bfd.h"

extern struct thread_master *master;

/* All information about zebra. */
struct zclient *zclient = NULL;

/* bfdd's interface node. */
struct cmd_node interface_node = {
  INTERFACE_NODE,
  "%s(config-if)# ",
  1
};

void
bfd_zclient_reset (void)
{
  zclient_reset (zclient);
};


DEFUN (bfd_interval,
       bfd_interval_cmd,
       "bfd interval <200-30000> min_rx <200-30000> multiplier <1-20>",
       "BFD configuration\n"
       "desired transmit interval\n"
       "msec\n"
       "required minimum receive interval\n"
       "msec\n" "detection multiplier\n")
{
  struct bfd_if_info *bii =
    (struct bfd_if_info *) ((struct interface *) vty->index)->info;

  u_int32_t interval = BFD_IF_INTERVAL_DFT;
  u_int32_t minrx = BFD_IF_MINRX_DFT;
  u_int32_t multi = BFD_IF_MULTIPLIER_DFT;

  interval = atoi (argv[0]);
  minrx = atoi (argv[1]);
  multi = atoi (argv[2]);

  if ((interval < BFD_IF_INTERVAL_MIN) || (interval > BFD_IF_INTERVAL_MAX))
    {
      vty_out (vty, "Interval is invalid%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if ((minrx < BFD_IF_MINRX_MIN) || (minrx > BFD_IF_MINRX_MAX))
    {
      vty_out (vty, "Min_rx is invalid%s", VTY_NEWLINE);
      return CMD_WARNING;
    }
  if ((multi < BFD_IF_MULTIPLIER_MIN) || (multi > BFD_IF_MULTIPLIER_MAX))
    {
      vty_out (vty, "Multiplier is invalid%s", VTY_NEWLINE);
      return CMD_WARNING;
    }

  bii->interval = interval;
  bii->minrx = minrx;
  bii->multiplier = multi;

  return CMD_SUCCESS;
};

static int
bfd_passive_interface (struct vty *vty, int set)
{
  struct bfd_if_info *bii =
    (struct bfd_if_info *) ((struct interface *) vty->index)->info;
  if (bii)
    {
      bii->passive = set;
      return CMD_SUCCESS;
    }
  return CMD_WARNING;
}

DEFUN (bfd_passive,
       bfd_passive_cmd,
       "bfd passive",
       "BFD configuration\n" "Don't send BFD control packets first.\n")
{
  return bfd_passive_interface (vty, 1);
};

DEFUN (no_bfd_passive,
       no_bfd_passive_cmd,
       "no bfd passive",
       NO_STR "BFD configuration\n" "Don't send BFD control packets first.\n")
{
  return bfd_passive_interface (vty, 0);
};


DEFUN (show_bfd_neighbors,
       show_bfd_neighbors_cmd,
       "show bfd neighbors", SHOW_STR BFD_STR "Show BFD neighbors\n")
{
  bfd_sh_bfd_neigh (vty, BFD_SH_NEIGH);
  return CMD_SUCCESS;
};

DEFUN (show_bfd_neighbors_details,
       show_bfd_neighbors_details_cmd,
       "show bfd neighbors details", SHOW_STR BFD_STR "Show BFD neighbors\n")
{
  bfd_sh_bfd_neigh (vty, BFD_SH_NEIGH_DET);
  return CMD_SUCCESS;
};


void
bfd_sh_bfd_neigh_tbl (struct vty *vty, int mode,
		      struct route_table *neightable, int *header)
{
  struct route_node *node, *subnode;

  for (node = route_top (neightable); node != NULL; node = route_next (node))
    if (!node->info)
      continue;
    else
      for (subnode =
	   route_top (((struct bfd_addrtreehdr *) node->info)->info);
	   subnode != NULL; subnode = route_next (subnode))
	if (!subnode->info)
	  continue;
	else
	  {
	    char buf[INET6_ADDRSTRLEN];
	    char rbuf[INET6_ADDRSTRLEN];
	    char lbuf[INET6_ADDRSTRLEN];

	    struct bfd_neigh *neighp = (struct bfd_neigh *) subnode->info;

	    snprintf (lbuf, sizeof (lbuf), "%s",
		      sockunion2str (neighp->su_local, buf, sizeof (buf)));
	    snprintf (rbuf, sizeof (rbuf), "%s",
		      sockunion2str (neighp->su_remote, buf, sizeof (buf)));

	    if (*header)
	      {
		vty_out (vty,
			 "OutAddr          NeighAddr         LD/RD Holdown(mult) State     Int%s",
			 VTY_NEWLINE);
		*header = 0;
	      }
	    vty_out (vty, "%-16s %-16s %3u/%-3u %4u(%d) %9s %8s%s",
		     lbuf, rbuf, neighp->ldisc, neighp->rdisc,
		     MSEC (neighp->dtime), neighp->rmulti,
		     bfd_state_str[neighp->lstate],
		     ifindex2ifname (neighp->ifindex), VTY_NEWLINE);

	    if (mode == BFD_SH_NEIGH_DET)
	      {
		vty_out (vty,
			 "Local Diag: %u, Demand mode: %u, Poll bit: %u%s",
			 neighp->ldiag, bfd_neigh_check_lbit_d (neighp),
			 bfd_neigh_check_lbit_p (neighp), VTY_NEWLINE);
		vty_out (vty, "MinTxInt: %u, MinRxInt: %u, Multiplier: %u%s",
			 neighp->ldesmintx, neighp->lreqminrx, neighp->lmulti,
			 VTY_NEWLINE);
		vty_out (vty,
			 "Received MinRxInt: %u, Received Multiplier: %u%s",
			 neighp->rreqminrx, neighp->rmulti, VTY_NEWLINE);
		vty_out (vty,
			 "Holdown (hits): %u(%u), Hello (hits): %u(%u)%s",
			 MSEC (neighp->dtime), neighp->timer_cnt,
			 MSEC (neighp->negrxint), neighp->recv_cnt,
			 VTY_NEWLINE);
		vty_out (vty, "Rx Count: %u%s", neighp->recv_cnt,
			 VTY_NEWLINE);
		vty_out (vty, "Tx Count: %u%s", neighp->xmit_cnt,
			 VTY_NEWLINE);
		vty_out (vty,
			 "Last packet: Version: %u               - Diagnostic: %u%s",
			 neighp->rver, neighp->rdiag, VTY_NEWLINE);
		vty_out (vty,
			 "             State bit: %-9s     - Demand bit: %u%s",
			 bfd_state_str[neighp->rstate],
			 bfd_neigh_check_rbit_d (neighp), VTY_NEWLINE);
		vty_out (vty,
			 "             Poll bit: %-5u          - Final bit: %u%s",
			 bfd_neigh_check_rbit_p (neighp),
			 bfd_neigh_check_rbit_f (neighp), VTY_NEWLINE);
		vty_out (vty,
			 "             Multiplier: %-5u        - Length: %u%s",
			 neighp->rmulti, neighp->rlen, VTY_NEWLINE);
		vty_out (vty,
			 "             My Discr: %-5u          - Your Discr: %-5u%s",
			 neighp->ldisc, neighp->rdisc, VTY_NEWLINE);
		vty_out (vty,
			 "             Min tx interval: %-7u - Min rx interval: %u%s",
			 neighp->rdesmintx, neighp->rreqminrx, VTY_NEWLINE);
		vty_out (vty, "             Min Echo interval: %u%s%s",
			 neighp->rreqminechorx, VTY_NEWLINE, VTY_NEWLINE);
	      }
	  }
}

void
bfd_sh_bfd_neigh (struct vty *vty, int mode)
{
  int header = 1;
  bfd_sh_bfd_neigh_tbl (vty, mode, neightbl->v4->raddr, &header);
#ifdef HAVE_IPV6
  bfd_sh_bfd_neigh_tbl (vty, mode, neightbl->v6->raddr, &header);
#endif /* HAVE_IPV6 */
}


/* Configuration write function for bfdd. */
static int
config_write_interface (struct vty *vty)
{
  int write = 0;
  struct listnode *node;
  struct interface *ifp;
  struct bfd_if_info *bii;

  for (ALL_LIST_ELEMENTS_RO (iflist, node, ifp))
    {
      /* IF name */
      vty_out (vty, "interface %s%s", ifp->name, VTY_NEWLINE);
      write++;
      /* IF desc */
      if (ifp->desc)
	{
	  vty_out (vty, " description %s%s", ifp->desc, VTY_NEWLINE);
	  write++;
	}
      if (ifp->info)
	{
	  bii = ifp->info;
	  if ((bii->interval != BFD_IF_INTERVAL_DFT)
	      || (bii->minrx != BFD_IF_MINRX_DFT)
	      || (bii->multiplier != BFD_IF_MULTIPLIER_DFT))
	    vty_out (vty, " bfd interval %u min_rx %u multiplier %u%s",
		     bii->interval, bii->minrx, bii->multiplier, VTY_NEWLINE);
	  if (bii->passive)
	    vty_out (vty, " bfd passive%s", VTY_NEWLINE);
	}
    }
  return 0;
}

static int
ipv4_bfd_neigh_up (int command, struct zclient *zclient, zebra_size_t length)
{
  struct bfd_cneigh *cneighp;

  cneighp = ipv4_bfd_neigh_updown_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv4_bfd_neigh_up")
      bfd_cneigh_free (cneighp);
  return 0;
}

static int
ipv4_bfd_neigh_down (int command, struct zclient *zclient,
		     zebra_size_t length)
{
  struct bfd_cneigh *cneighp;

  cneighp = ipv4_bfd_neigh_updown_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv4_bfd_neigh_down")
      bfd_cneigh_free (cneighp);
  return 0;
}

static int
ipv4_bfd_cneigh_add (int command, struct zclient *zclient,
		     zebra_size_t length)
{
  int ret;
  struct bfd_cneigh *cneighp;

  cneighp = ipv4_bfd_cneigh_adddel_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv4_bfd_cneigh_add")
      ret = bfd_neigh_add (bfd_cneigh_to_neigh (cneighp));
  bfd_cneigh_free (cneighp);
  return ret;

}

static int
ipv4_bfd_cneigh_del (int command, struct zclient *zclient,
		     zebra_size_t length)
{
  int ret;
  struct bfd_cneigh *cneighp;

  cneighp = ipv4_bfd_cneigh_adddel_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv4_bfd_cneigh_del")
      ret = bfd_cneigh_del (cneighp);
  bfd_cneigh_free (cneighp);
  return ret;
}


#ifdef HAVE_IPV6
static int
ipv6_bfd_neigh_up (int command, struct zclient *zclient, zebra_size_t length)
{
  struct bfd_cneigh *cneighp;

  cneighp = ipv6_bfd_neigh_updown_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv6_bfd_neigh_up")
      bfd_cneigh_free (cneighp);
  return 0;
}

static int
ipv6_bfd_neigh_down (int command, struct zclient *zclient,
		     zebra_size_t length)
{
  struct bfd_cneigh *cneighp;

  cneighp = ipv6_bfd_neigh_updown_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv6_bfd_neigh_down")
      bfd_cneigh_free (cneighp);
  return 0;
}

static int
ipv6_bfd_cneigh_add (int command, struct zclient *zclient,
		     zebra_size_t length)
{
  int ret;
  struct bfd_cneigh *cneighp;

  cneighp = ipv6_bfd_cneigh_adddel_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv6_bfd_cneigh_add")
      ret = bfd_neigh_add (bfd_cneigh_to_neigh (cneighp));
  bfd_cneigh_free (cneighp);
  return ret;

}

static int
ipv6_bfd_cneigh_del (int command, struct zclient *zclient,
		     zebra_size_t length)
{
  int ret;
  struct bfd_cneigh *cneighp;

  cneighp = ipv6_bfd_cneigh_adddel_read (zclient->ibuf);

  if (BFD_IF_DEBUG_ZEBRA)
    BFD_ZEBRA_LOG_DEBUG_NOARG ("rcvd: ipv6_bfd_cneigh_del")
      ret = bfd_cneigh_del (cneighp);
  bfd_cneigh_free (cneighp);
  return ret;
}
#endif /* HAVE_IPV6 */


static int
bfd_interface_add (int command, struct zclient *zclient, zebra_size_t length,
 vrf_id_t vrf_id)
{
  struct interface *ifp;

  ifp = zebra_interface_add_read (zclient->ibuf, vrf_id);

  if (BFD_IF_DEBUG_ZEBRA)
    zlog_debug ("Zebra rcvd: interface add %s", ifp->name);

  return 0;
}

static int
bfd_interface_delete (int command, struct zclient *zclient,
		      zebra_size_t length, vrf_id_t vrf_id)
{
  struct stream *s;
  struct interface *ifp;

  s = zclient->ibuf;
  ifp = zebra_interface_state_read (s, vrf_id);
  ifp->ifindex = IFINDEX_INTERNAL;

  if (BFD_IF_DEBUG_ZEBRA)
    zlog_debug ("Zebra rcvd: interface delete %s", ifp->name);

  return 0;
}

static int
bfd_interface_up (int command, struct zclient *zclient, zebra_size_t length,
 vrf_id_t vrf_id)
{
  struct stream *s;
  struct interface *ifp;

  s = zclient->ibuf;
  ifp = zebra_interface_state_read (s, vrf_id);

  if (!ifp)
    return 0;

  if (BFD_IF_DEBUG_ZEBRA)
    zlog_debug ("Zebra rcvd: interface %s up", ifp->name);

  return 0;
}

static int
bfd_interface_down (int command, struct zclient *zclient, zebra_size_t length,
 vrf_id_t vrf_id)
{
  struct stream *s;
  struct interface *ifp;

  s = zclient->ibuf;
  ifp = zebra_interface_state_read (s, vrf_id);
  if (!ifp)
    return 0;

  if (BFD_IF_DEBUG_ZEBRA)
    zlog_debug ("Zebra rcvd: interface %s down", ifp->name);
  return 0;
}

static int
bfd_interface_address_add (int command, struct zclient *zclient,
			   zebra_size_t length, vrf_id_t vrf_id)
{
  struct connected *ifc;

  ifc = zebra_interface_address_read (command, zclient->ibuf, vrf_id);

  if (ifc == NULL)
    return 0;

  if (BFD_IF_DEBUG_ZEBRA)
    {
      char buf[128];
      prefix2str (ifc->address, buf, sizeof (buf));
      zlog_debug ("Zebra rcvd: interface %s address add %s",
		  ifc->ifp->name, buf);
    }
  return 0;
}

static int
bfd_interface_address_delete (int command, struct zclient *zclient,
			      zebra_size_t length, vrf_id_t vrf_id)
{
  struct connected *ifc;

  ifc = zebra_interface_address_read (command, zclient->ibuf, vrf_id);

  if (ifc == NULL)
    return 0;

  if (BFD_IF_DEBUG_ZEBRA)
    {
      char buf[128];
      prefix2str (ifc->address, buf, sizeof (buf));
      zlog_debug ("Zebra rcvd: interface %s address delete %s",
		  ifc->ifp->name, buf);
    }

  connected_free (ifc);

  return 0;
}

void
bfd_signal_neigh_updown (struct bfd_neigh *neighp, int cmd)
{
  struct prefix *raddr = sockunion2hostprefix (neighp->su_remote, NULL);
  struct prefix *laddr = sockunion2hostprefix (neighp->su_local, NULL);

  if (bfd_check_neigh_family (neighp) == AF_INET)
    zapi_ipv4_bfd_neigh_updown (zclient,
				cmd,
				(struct prefix_ipv4 *) raddr,
				(struct prefix_ipv4 *) laddr,
				neighp->ifindex);
#ifdef HAVE_IPV6
  else
    zapi_ipv6_bfd_neigh_updown (zclient,
				cmd,
				(struct prefix_ipv6 *) raddr,
				(struct prefix_ipv6 *) laddr,
				neighp->ifindex);
#endif /* HAVE_IPV6 */
   prefix_free(raddr);
   prefix_free(laddr); 
}



/* Initialization of BFD interface. */
static void
bfd_vty_cmd_init (void)
{

  /* Initialize Zebra interface data structure */
  //if_init ();

  /* Install interface node. */
  install_node (&interface_node, config_write_interface);

  install_element (VIEW_NODE, &show_bfd_neighbors_details_cmd);
  install_element (ENABLE_NODE, &show_bfd_neighbors_details_cmd);

  install_element (VIEW_NODE, &show_bfd_neighbors_cmd);
  install_element (ENABLE_NODE, &show_bfd_neighbors_cmd);

  install_element (CONFIG_NODE, &interface_cmd);
  install_element (CONFIG_NODE, &no_interface_cmd);
  install_default (INTERFACE_NODE);
  install_element (INTERFACE_NODE, &bfd_interval_cmd);
  install_element (INTERFACE_NODE, &bfd_passive_cmd);
  install_element (INTERFACE_NODE, &no_bfd_passive_cmd);
};


void
bfd_vty_init (void)
{
  bfd_vty_cmd_init ();
  bfd_vty_debug_init ();
};

static void
bfd_zebra_connected (struct zclient *zclient)
{
  zclient_send_requests (zclient, VRF_DEFAULT);
}

void
bfd_zebra_init (struct thread_master *master)
{
  zclient = zclient_new (master);
  zclient_init (zclient, ZEBRA_ROUTE_BFD);	/* FIXME */
  zclient->flags = ZCLIENT_FLAGS_BFDD;

  /* Callback functions */
  zclient->zebra_connected = bfd_zebra_connected;
  zclient->interface_add = bfd_interface_add;
  zclient->interface_delete = bfd_interface_delete;
  zclient->interface_address_add = bfd_interface_address_add;
  zclient->interface_address_delete = bfd_interface_address_delete;
  zclient->interface_up = bfd_interface_up;
  zclient->interface_down = bfd_interface_down;
  zclient->ipv4_bfd_cneigh_add = ipv4_bfd_cneigh_add;
  zclient->ipv4_bfd_cneigh_del = ipv4_bfd_cneigh_del;
  zclient->ipv4_bfd_neigh_up = ipv4_bfd_neigh_up;
  zclient->ipv4_bfd_neigh_down = ipv4_bfd_neigh_down;
#ifdef HAVE_IPV6
  zclient->ipv6_bfd_cneigh_add = ipv6_bfd_cneigh_add;
  zclient->ipv6_bfd_cneigh_del = ipv6_bfd_cneigh_del;
  zclient->ipv6_bfd_neigh_up = ipv6_bfd_neigh_up;
  zclient->ipv6_bfd_neigh_down = ipv6_bfd_neigh_down;
#endif /* HAVE_IPV6 */

  }
