/**************************************
 ecmh - Easy Cast du Multi Hub
 by Jeroen Massar <jeroen@unfix.org>
***************************************
 $Author: fuzzel $
 $Id: ecmh.c,v 1.9 2004/02/17 19:05:13 fuzzel Exp $
 $Date: 2004/02/17 19:05:13 $
**************************************/
//
// Docs to check:
//	netdevice(7), packet(7)
//	RFC 2710 - Multicast Listener Discovery (MLD) for IPv6
//	RFC 3569 - An Overview of Source-Specific Multicast (SSM)
//	RFC 3590 - Source Address Selection for the Multicast Listener Discovery (MLD) Protocol
//	RFC 3678 - Socket Interface Extensions for Multicast Source Filters
//
//	http://www.ietf.org/internet-drafts/draft-vida-mld-v2-08.txt
//		Multicast Listener Discovery Version 2 (MLDv2) for IPv6
//	http://www.ietf.org/internet-drafts/draft-holbrook-idmr-igmpv3-ssm-05.txt
//		Using IGMPv3 and MLDv2 For Source-Specific Multicast
//
// - Protocol Robustness, send twice first with S flag, second without.
// - Querier Election support (MLDv2 7.6.2 + 7.1)
//

#include "ecmh.h"

// Configuration Variables
struct conf	*g_conf;
volatile int	g_needs_timeout = false;

/**************************************
  Functions
**************************************/

// Send a packet
void sendpacket6(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len)
{
	struct sockaddr_ll	sa;
	int			sent,i;

	memset(&sa, 0, sizeof(sa));

	sa.sll_family	= AF_PACKET;
	sa.sll_protocol	= htons(ETH_P_IPV6);
	sa.sll_ifindex	= intn->ifindex;
	sa.sll_hatype	= intn->hwaddr.sa_family;
	sa.sll_pkttype	= 0;
	sa.sll_halen	= 6;

	// Construct a Ethernet MAC address from the IPv6 destination multicast address.
	// Per RFC2464
	sa.sll_addr[0] = 0x33;
	sa.sll_addr[1] = 0x33;
	sa.sll_addr[2] = iph->ip6_dst.s6_addr[12];
	sa.sll_addr[3] = iph->ip6_dst.s6_addr[13];
	sa.sll_addr[4] = iph->ip6_dst.s6_addr[14];
	sa.sll_addr[5] = iph->ip6_dst.s6_addr[15];

	// Resend the packet
	errno = 0;
	sent = sendto(g_conf->rawsocket, iph, len, 0, (struct sockaddr *)&sa, sizeof(sa));
	if (sent < 0)
	{
		// Remove the device if it doesn't exist anymore, can happen with dynamic tunnels etc
		if (errno == ENXIO)
		{
			// Delete from the list
			listnode_delete(g_conf->ints, intn);
			// Destroy the interface itself
			int_destroy(intn);
		}
		else dolog(LOG_WARNING, "[%-5s] sending %d bytes failed, mtu = %d, %d: %s\n", intn->name, len, intn->mtu, errno, strerror(errno));
		return;
	}

	// Update the global statistics
	g_conf->stat_packets_sent++;
	g_conf->stat_bytes_sent+=len;

	// Update interface statistics
	intn->stat_bytes_sent+=len;
	intn->stat_packets_sent++;
	return;
}

uint16_t inchksum(const void *data, uint32_t length)
{
	register long		sum = 0;
	register const uint16_t *wrd = (const uint16_t *)data;
	register long		slen = (long)length;

	while (slen > 1)
	{
		sum += *wrd++;
		slen-=2;
	}

	if (slen > 0) sum+=*(const uint8_t *)wrd;

	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)sum;
}

uint16_t ipv6_checksum(const struct ip6_hdr *ip6, uint8_t protocol, const void *data, const uint16_t length)
{
	struct
	{
		uint16_t	length;
		uint16_t	zero1;
		uint8_t		zero2;
		uint8_t		next;
	} pseudo;
	register uint32_t	chksum = 0;

	pseudo.length	= htons(length);
	pseudo.zero1	= 0;
	pseudo.zero2	= 0;
	pseudo.next	= protocol;

	// IPv6 Source + Dest
	chksum  = inchksum(&ip6->ip6_src, sizeof(ip6->ip6_src) + sizeof(ip6->ip6_dst));
	chksum += inchksum(&pseudo, sizeof(pseudo));
	chksum += inchksum(data, length);

	// Wrap in the carries to reduce chksum to 16 bits.
	chksum  = (chksum >> 16) + (chksum & 0xffff);
	chksum += (chksum >> 16);

	// Take ones-complement and replace 0 with 0xFFFF.
	chksum = (uint16_t) ~chksum;
	if (chksum == 0UL) chksum = 0xffffUL;
	return (uint16_t)chksum;
}

/*
 * This is used for the ICMPv6 reply code, to allow sending Hoplimit's :)
 * Thus allowing neat tricks like traceroute6's to work.
 */
void icmp6_send(struct intnode *intn, const struct in6_addr *src, int type, int code, void *data, unsigned int dlen)
{
	struct icmp6_hoplimit_packet
	{
		struct ip6_hdr		ip6;
		struct icmp6_hdr	icmp6;
		char			data[1500];
		
	} packet;

	memset(&packet, 0, sizeof(packet));

	// Create the IPv6 packet
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_plen		= ntohs(sizeof(packet) - (sizeof(packet.data) - dlen) - sizeof(packet.icmp6.icmp6_data32) - sizeof(packet.ip6));
	packet.ip6.ip6_nxt		= IPPROTO_ICMPV6;
	// Hoplimit of 64 seems to be a semi default
	packet.ip6.ip6_hlim		= 64;

	// The source address must be a global unicast IPv6 address
	// and should be associated to the interface we are sending on
	memcpy(&packet.ip6.ip6_src, &intn->global, sizeof(packet.ip6.ip6_src));

	// Target == Sender
	memcpy(&packet.ip6.ip6_dst, src, sizeof(*src));

	// ICMPv6 Error Report
	packet.icmp6.icmp6_type		= type;
	packet.icmp6.icmp6_code		= code;
	
	// Add the data, we start at the data in the icmp6 packet
	memcpy(&packet.icmp6.icmp6_data32, data, (sizeof(packet.data) > dlen ? dlen : sizeof(packet.data)));

	// Calculate and fill in the checksum
	packet.icmp6.icmp6_cksum	= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.icmp6, sizeof(packet.icmp6) + dlen - sizeof(packet.icmp6.icmp6_data32));

	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet) - (sizeof(packet.data) - dlen) - sizeof(packet.icmp6.icmp6_data32));

	// Increase ICMP sent statistics
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}

/* MLDv1 and MLDv2 are backward compatible when doing Queries
   aka a router implementing MLDv2 can send the same query
   and both MLDv1 and MLDv2 hosts will understand it.
   MLDv2 hosts will return a MLDv2 report, MLDv1 hosts a MLDv1 report
*/
void mld_send_query(struct intnode *intn, const struct in6_addr *mca)
{
	struct mld_query_packet
	{
		struct ip6_hdr		ip6;
		struct ip6_hbh		hbh;
		struct
		{
			uint8_t		type;
			uint8_t		length;
			uint16_t	value;
			uint8_t		optpad[2];
		}			routeralert;
#ifdef ECMH_SUPPORT_MLD2
		struct mld2_query	mldq;
#else
		struct mld1		mldq;
#endif
		
	} packet;

	memset(&packet, 0, sizeof(packet));

	// Create the IPv6 packet
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_plen		= ntohs(sizeof(packet) - sizeof(packet.ip6));
	packet.ip6.ip6_nxt		= IPPROTO_HOPOPTS;
	packet.ip6.ip6_hlim		= 1;

	// The source address must be the link-local address
	// of the interface we are sending on
	memcpy(&packet.ip6.ip6_src, &intn->linklocal, sizeof(packet.ip6.ip6_src));

	// Generaly Query -> link-scope all-nodes (ff02::1)
	packet.ip6.ip6_dst.s6_addr[0]	= 0xff;
	packet.ip6.ip6_dst.s6_addr[1]	= 0x02;
	packet.ip6.ip6_dst.s6_addr[15]	= 0x01;

	// HopByHop Header Extension
	packet.hbh.ip6h_nxt		= IPPROTO_ICMPV6;
	packet.hbh.ip6h_len		= 0;
	
	// Router Alert Option
	packet.routeralert.type		= 5;
	packet.routeralert.length	= sizeof(packet.routeralert.value);
	packet.routeralert.value	= 0;			// MLD ;)

	// Option Padding
	packet.routeralert.optpad[0]	= IP6OPT_PADN;
	packet.routeralert.optpad[1]	= 0;

	// ICMPv6 MLD Query
	packet.mldq.type		= ICMP6_MEMBERSHIP_QUERY;
	packet.mldq.mrc			= htons(2000);

	// The address to query, can be in6addr_any to
	// query for everything or a specific group 
	memcpy(&packet.mldq.mca, mca, sizeof(*mca));

#ifdef ECMH_SUPPORT_MLD2
	packet.mldq.nsrcs		= 0;
#endif

	// Calculate and fill in the checksum
	packet.mldq.csum		= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.mldq, sizeof(packet.mldq));

	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet));

	// Increase ICMP sent statistics
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}

void mld1_send_report(struct intnode *intn, const struct in6_addr *mca)
{
	struct mld_report_packet
	{
		struct ip6_hdr		ip6;
		struct ip6_hbh		hbh;
		struct
		{
			uint8_t		type;
			uint8_t		length;
			uint16_t	value;
			uint8_t		optpad[2];
		}			routeralert;
		
		struct mld1		mld1;
		
	} packet;

	memset(&packet, 0, sizeof(packet));

	// Create the IPv6 packet
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_plen		= ntohs(sizeof(packet) - sizeof(packet.ip6));
	packet.ip6.ip6_nxt		= IPPROTO_HOPOPTS;
	packet.ip6.ip6_hlim		= 1;

	// The source address must be the link-local address
	// of the interface we are sending on
	memcpy(&packet.ip6.ip6_src, &intn->linklocal, sizeof(packet.ip6.ip6_src));

	// Report -> Multicast address
	memcpy(&packet.ip6.ip6_dst, mca, sizeof(*mca));

	// HopByHop Header Extension
	packet.hbh.ip6h_nxt		= IPPROTO_ICMPV6;
	packet.hbh.ip6h_len		= 0;

	// Router Alert Option
	packet.routeralert.type		= 5;
	packet.routeralert.length	= sizeof(packet.routeralert.value);
	packet.routeralert.value	= 0;			// MLD ;)

	// Option Padding
	packet.routeralert.optpad[0]	= IP6OPT_PADN;
	packet.routeralert.optpad[1]	= 0;

	// ICMPv6 MLD Report
	packet.mld1.type		= ICMP6_MEMBERSHIP_REPORT;
	packet.mld1.mrc			= 0;
	memcpy(&packet.mld1.mca, mca, sizeof(*mca));

	// Calculate and fill in the checksum
	packet.mld1.csum		= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.mld1, sizeof(packet.mld1));

	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet));

	// Increase ICMP sent statistics
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}

#ifdef ECMH_SUPPORT_MLD2
void mld2_send_report(struct intnode *intn, const struct in6_addr *mca)
{
	struct mld_report_packet
	{
		struct ip6_hdr		ip6;
		struct ip6_hbh		hbh;
		struct
		{
			uint8_t		type;
			uint8_t		length;
			uint16_t	value;
			uint8_t		optpad[2];
		}			routeralert;
		
		struct mld1		mld1;
		
	} packet;

	memset(&packet, 0, sizeof(packet));

	// Create the IPv6 packet
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_plen		= ntohs(sizeof(packet) - sizeof(packet.ip6));
	packet.ip6.ip6_nxt		= IPPROTO_HOPOPTS;
	packet.ip6.ip6_hlim		= 1;

	// The source address must be the link-local address
	// of the interface we are sending on
	memcpy(&packet.ip6.ip6_src, &intn->linklocal, sizeof(packet.ip6.ip6_src));

	// Report -> Multicast address
	memcpy(&packet.ip6.ip6_dst, mca, sizeof(*mca));

	// HopByHop Header Extension
	packet.hbh.ip6h_nxt		= IPPROTO_ICMPV6;
	packet.hbh.ip6h_len		= 0;

	// Router Alert Option
	packet.routeralert.type		= 5;
	packet.routeralert.length	= sizeof(packet.routeralert.value);
	packet.routeralert.value	= 0;			// MLD ;)

	// Option Padding
	packet.routeralert.optpad[0]	= IP6OPT_PADN;
	packet.routeralert.optpad[1]	= 0;

	// ICMPv6 MLD Report
	packet.mld1.type		= ICMP6_MEMBERSHIP_REPORT;
	packet.mld1.mrc			= 0;
	memcpy(&packet.mld1.mca, mca, sizeof(*mca));

	// NOTE, we need to watch the MTU size, as there
	// will be more subscriptions + SSM combo's than
	// the size of one MTU can fit -> split it when needed.
	
	// Count the number of groups + sources to be send
	LIST_LOOP(g_conf->groups, groupn, ln)
	{
		// If we only need to send for this MCA
		// don't count anything else
		if (	(mca && IN6_ARE_ADDR_EQUAL(mca, &group->mca)) ||
			groupn->interfaces->count == 0) continue;
		
		this_srcs = 0;

		LIST_LOOP(groupn->interfaces, grpintn, gn)
		{
			// Skip the sending interface
			if (grpintn->interface == intn) continue;

			// Count the source subscriptions
			this_srcs++;
		}
		
		if (this_srcs > 0)
		{
			groups++;
			srcs+=this_srcs;
		}
	}
	
	// Allocate memory to let the above all fit
	
	// Copy the header in front of it
		
	// Loop again and append the sources + headers
	LIST_LOOP(g_conf->groups, groupn, ln)
	{
		// If we only need to send for this MCA
		// don't count anything else
		if (	(mca && IN6_ARE_ADDR_EQUAL(mca, &group->mca)) ||
			groupn->interfaces->count == 0) continue;

		// We haven't added a group record yet
		grec_added = 0;

		LIST_LOOP(groupn->interfaces, grpintn, gn)
		{
			// Skip the sending interface
			if (grpintn->interface == intn) continue;

			// Add a group record if we didn't do so already
			if (grec_added == 0)
			{
			}

			// Copy the address into the packet
		}
		
		// Fixup the number of addresses
	}

	// Calculate and fill in the checksum
	packet.mld1.csum		= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.mld1, sizeof(packet.mld1));

	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet));

	// Increase ICMP sent statistics
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}
#endif // ECMH_SUPPORT_MLD2

void mld_send_report(struct intnode *intn, const struct in6_addr *mca)
{
#ifdef ECMH_SUPPORT_MLD2
	// When we haven't detected a querier on a link
	// send reports as MLDv1's as listeners (hosts)
	// might be interrested.
	if (intn->mld_version == 0 || intn->mld_version == 1)
#endif // EMCH_SUPPORT_MLD2
		mld1_send_report(intn, mca);
#ifdef ECMH_SUPPORT_MLD2
	else if (intn->mld_version == 2)
		mld2_send_report(intn, mca);
#endif // EMCH_SUPPORT_MLD2
}

#ifdef ECMH_SUPPORT_IPV4

// IPv4 ICMP
void l4_ipv4_icmp(struct intnode *intn, struct ip *iph, const uint8_t *packet, const uint16_t len)
{
	D(dolog(LOG_DEBUG, "%5s L4:IPv4 ICMP\n", intn->name);)
	return;
}

// Protocol 41 - IPv6 in IPv4 (RFC3065)
void l4_ipv4_proto41(struct intnode *intn, struct ip *iph, const uint8_t *packet, const uint16_t len)
{
	D(dolog(LOG_DEBUG, "%5s L4:IPv4 Proto 41\n", intn->name);)
	return;
}

// IPv4
void l3_ipv4(struct intnode *intn, const uint8_t *packet, const uint16_t len)
{
	struct ip *iph;
D(	char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];)

	// Ignore IPv4 ;)
	return;

	iph = (struct ip *)packet;

	if (iph->ip_v != 4)
	{
		D(dolog(LOG_DEBUG, "%5s L3:IPv4: IP version %d not supported\n", intn->name, iph->ip_v);)
		return;
	}

	if (iph->ip_hl < 5)
	{
		D(dolog(LOG_DEBUG, "%5s L3IPv4: IP hlen < 5 bytes (%d)\n", intn->name, iph->ip_hl);)
		return;
	}

	if (ntohs (iph->ip_len) != len)
	{
		// This happens mostly with unknown ARPHRD_* types
		D(dolog(LOG_DEBUG, "%5s L3:IPv4: *** L3 length != L2 length (%d != %d)\n", intn->name, ntohs(iph->ip_len), len);)
//		return;
	}

D(
	inet_ntop(AF_INET, &iph->ip_src, src, sizeof(src));
	inet_ntop(AF_INET, &iph->ip_dst, dst, sizeof(dst));

	dolog(LOG_DEBUG, "%5s L3:IPv4: IPv%01u %-16s %-16s %4u\n", intn->name, iph->ip_v, src, dst, ntohs(iph->ip_len));
)

	// Go to Layer 4
	if (iph->ip_p == 1) l4_ipv4_icmp(intn, iph, packet+4*iph->ip_hl, len-4*iph->ip_hl);
	// IGMP ??? if (iph->ip_p ==...)
	else if (iph->ip_p == 41) l4_ipv4_proto41(intn, iph, packet+4*iph->ip_hl, len-4*iph->ip_hl);
}

#endif // ECMH_SUPPORT_IPV4

void mld_warning(char *fmt, struct in6_addr *i_mca, const struct intnode *intn)
{
	char mca[INET6_ADDRSTRLEN];
	memset(mca,0,sizeof(mca));
	inet_ntop(AF_INET6, i_mca, mca, sizeof(mca));
	dolog(LOG_WARNING, fmt, mca, intn->name);
}

void l4_ipv6_icmpv6_mld1_report(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld1 *mld1, const uint16_t plen)
{
	struct grpintnode	*grpintn;
	struct in6_addr		any;

	// We have received a MLDv1 report, thus note this
	// interface as having a MLDv1 listener
	int_set_mld_version(intn, 1);

	D(dolog(LOG_DEBUG, "Received a ICMPv6 MLDv1 Report on %s\n", intn->name);)

	// Ignore groups:
	// - non multicast 
	// - node local multicast addresses
	// - link local multicast addresses
	// - multicast destination mismatch with ipv6 destination
	if (	!IN6_IS_ADDR_MULTICAST(&mld1->mca) ||
		IN6_IS_ADDR_MC_NODELOCAL(&mld1->mca) ||
		IN6_IS_ADDR_MC_LINKLOCAL(&mld1->mca)) return;

	// Find the grpintnode or create it
	grpintn = groupint_get(&mld1->mca, intn);
	if (!grpintn)
	{
		mld_warning("Couldn't find or create new group %s for %s\n", &mld1->mca, intn);
		return;
	}

	// No source address, so use any
	memset(&any,0,sizeof(any));
	
	if (!grpint_refresh(grpintn, &any, MLD_SSM_MODE_INCLUDE))
	{
		mld_warning("Couldn't create subscription to %s for %s\n", &mld1->mca, intn);
		return;
	}

	return;
}

void l4_ipv6_icmpv6_mld1_reduction(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld1 *mld1, const uint16_t plen)
{
	struct groupnode	*groupn;
	struct grpintnode	*grpintn;
	struct subscrnode	*subscrn;
	struct in6_addr		any;

	// We have received a MLDv1 reduction, thus note this
	// interface as having a MLDv1 listener
	int_set_mld_version(intn, 1);

	D(dolog(LOG_DEBUG, "Received a ICMPv6 MLDv1 Reduction on %s\n", intn->name);)

	// Ignore groups:
	// - non multicast 
	// - node local multicast addresses
	// - link local multicast addresses
	if (	!IN6_IS_ADDR_MULTICAST(&mld1->mca) ||
		IN6_IS_ADDR_MC_NODELOCAL(&mld1->mca) ||
		IN6_IS_ADDR_MC_LINKLOCAL(&mld1->mca)) return;

	// Find the groupnode
	groupn = group_find(&mld1->mca);
	if (!groupn)
	{
		mld_warning("Couldn't find group %s for reduction of %s\n", &mld1->mca, intn);
		return;
	}

	// Find the grpintnode
	grpintn = grpint_find(groupn->interfaces, intn);
	if (!grpintn)
	{
		mld_warning("Couldn't find the grpint %s for reduction of %s\n", &mld1->mca, intn);
		return;
	}

	// No source address, so use any
	memset(&any,0,sizeof(any));

	if (!subscr_unsub(grpintn->subscriptions, &any))
	{
		mld_warning("Couldn't unsubscribe from %s interface %s\n", &mld1->mca, intn);
		return;
	}
	
	// Requery if the list is suddenly empty, as it will timeout otherwise.
	D(mld_warning("Querying for other listeners of %s on interface %s\n", &mld1->mca, intn);)
	mld_send_query(intn, &mld1->mca);

	return;
}

#ifdef ECMH_SUPPORT_MLD2
void l4_ipv6_icmpv6_mld2_report(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld2_report *mld2r, const uint16_t plen)
{
	char			mca[INET6_ADDRSTRLEN], srct[INET6_ADDRSTRLEN];
	struct grpintnode	*grpintn = NULL;
	struct in6_addr		*src;
	struct mld2_grec	*grec = ((void *)mld2r) + sizeof(*mld2r);
	int ngrec		= ntohs(mld2r->ngrec);
	int nsrcs		= 0;

	// We have received a MLDv2 report, thus note this
	// interface as having a MLDv2 listener, unless it has detected MLDv1 listeners already...
	int_set_mld_version(intn, 2);

	D(dolog(LOG_DEBUG, "Received a ICMPv6 MLDv2 Report on %s\n", intn->name);)

	// Zero out just in case
	mca[0] = srct[0] = 0;

	while (ngrec > 0)
	{
		if (grec->grec_auxwords != 0)
		{
			dolog(LOG_WARNING, "%5s L4:IPv6:ICMPv6:MLD2_Report Auxwords was %d instead of required 0\n", intn->name, grec->grec_auxwords);
			return;
		}
		
	//	D(dolog(LOG_DEBUG, "MLDv2 Report for %s : ", mca);)

		// Ignore node and link local multicast addresses
		if (	!IN6_IS_ADDR_MC_NODELOCAL(&grec->grec_mca) &&
			!IN6_IS_ADDR_MC_LINKLOCAL(&grec->grec_mca))
		{
			inet_ntop(AF_INET6, &grec->grec_mca, mca, sizeof(mca));

			// Find the grpintnode or create it
			grpintn = groupint_get(&grec->grec_mca, intn);
			if (!grpintn) dolog(LOG_WARNING, "%5s L4:IPv6:ICMPv6:MLD2_Report Couldn't find or create new group for %s\n", intn->name, mca);
		}

		nsrcs = ntohs(grec->grec_nsrcs);
		src = ((void *)grec) + sizeof(*grec);
		// Do all source addresses
		while (nsrcs > 0)
		{
			// Skip if we didn't get a grpint
			if (grpintn)
			{
				inet_ntop(AF_INET6, src, srct, sizeof(srct));
				if (!grpint_refresh(grpintn, src)) dolog(LOG_WARNING, "Couldn't subscribe %s to %s<->\n", intn->name, mca, srct);
			}

			// next src
			src = ((void *)src) + sizeof(*src);
		}
		
		// next grec
		grec = ((void *)grec) + sizeof(*grec);
	}

	return;
}
#endif // ECMH_SUPPORT_MLD2

void l4_ipv6_icmpv6_mld_query(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld2_query *mld, const uint16_t plen)
{
	struct groupnode	*groupn;
	struct listnode		*ln;
	struct grpintnode	*grpintn;
	struct listnode		*gn;

	D(dolog(LOG_DEBUG, "Received a ICMPv6 MLD Query on %s\n", intn->name);)
	
	// It's MLDv1 when the packet has the size of an MLDv1 packet
	if (plen == sizeof(struct mld1)) int_set_mld_version(intn, 1);
	// It is MLDv2 (or up) when it has anything else
	// It could be MLDv3 if that ever comes out, but we don't know.
	else int_set_mld_version(intn, 2);

#ifdef ECMH_SUPPORT_MLD2
	// If it is a yet unknown MLD send MLDv1's
	// Unknown MLD's should not happen though
	// as the above code just determined what
	// version it is.
	if (intn->mld_version == 0 || intn->mld_version == 1)
	{
#endif // ECMH_SUPPORT_MLD2
		// MLDv1 sends reports one group at a time

		// Walk along the list of groups
		// and report all the groups we are subscribed for
		LIST_LOOP(g_conf->groups, groupn, ln)
		{
			LIST_LOOP(groupn->interfaces, grpintn, gn)
			{
				// We only are sending for this interface
				if (grpintn->interface != intn) continue;
	
				// Report this group to the querying router
				mld_send_report(intn, &groupn->mca);
			}
		}
#ifdef ECMH_SUPPORT_MLD2
	}
	else
	{
		mld2_send_report_all(intn);
	}
#endif // ECMH_SUPPORT_MLD2

	return;
}

// Forward a multicast packet to interfaces that have subscriptions for it
//
// intn		= The interface we received this packet on
// packet	= The packet, starting with IPv6 header
// len		= Length of the complete packet
void l4_ipv6_multicast(struct intnode *intn, struct ip6_hdr *iph, const uint16_t len)
{
	struct groupnode	*groupn;
	struct grpintnode	*grpintn;
	struct subscrnode	*subscrn;
	struct listnode		*in, *in2;

	// Don't route multicast packets that:
	// - src = multicast
	// - src = unspecified
	// - dst = unspecified
	// - src = linklocal
	// - dst = node local multicast
	// - dst = link local multicast
	if (	IN6_IS_ADDR_MULTICAST(&iph->ip6_src) ||
		IN6_IS_ADDR_UNSPECIFIED(&iph->ip6_src) ||
		IN6_IS_ADDR_UNSPECIFIED(&iph->ip6_dst) ||
		IN6_IS_ADDR_LINKLOCAL(&iph->ip6_src) ||
		IN6_IS_ADDR_MC_NODELOCAL(&iph->ip6_dst) ||
		IN6_IS_ADDR_MC_LINKLOCAL(&iph->ip6_dst)) return;
/*
D(
	{
	char			src[INET6_ADDRSTRLEN];
	char			dst[INET6_ADDRSTRLEN];
	inet_ntop(AF_INET6, &iph->ip6_src, src, sizeof(src));
	inet_ntop(AF_INET6, &iph->ip6_dst, dst, sizeof(dst));
	dolog(LOG_DEBUG, "%5s L3:IPv6: IPv%0x %40s %40s %4u %d\n", intn->name, (int)((iph->ip6_vfc>>4)&0x0f), src, dst, ntohs(iph->ip6_plen), iph->ip6_nxt);
	}
)
*/
	// Find the group belonging to this multicast destination
	groupn = group_find(&iph->ip6_dst);

	if (!groupn)
	{
		// DEBUG - Causes heavy debug output - thus disabled even for debugging
		//D(dolog(LOG_DEBUG, "No subscriptions for %s (sent by %s)\n",  dst, src);)
		return;
	}

	LIST_LOOP(groupn->interfaces, grpintn, in)
	{
		// Don't send to the interface this packet originated from
		if (intn->ifindex == grpintn->interface->ifindex) continue;

		// Check the subscriptions for this group
		LIST_LOOP(grpintn->subscriptions, subscrn, in2)
		{
			// Unspecified or specific subscription to this address?
			if (	IN6_ARE_ADDR_EQUAL(&subscrn->ipv6, &in6addr_any) ||
				IN6_ARE_ADDR_EQUAL(&subscrn->ipv6, &iph->ip6_src))
			{
				// If it was requested to exclude this one,
				// skip sending altogether
				if (subscrn->mode == MLD_SSM_MODE_EXCLUDE) break;

				// Send the packet
				sendpacket6(grpintn->interface, iph, len);
				
				// Even if there are multiple source subscriptions
				// we only send it once, let the router(s) & host(s)
				// on the link determine that they want it.
				break;
			}
		}
	}
}

// Check the ICMPv6 message for MLD's or ICMP echo requests
//
// intn		= The interface we received this packet on
// packet	= The packet, starting with IPv6 header
// len		= Length of the complete packet
// data		= the payload
// plen		= Payload length (should match up to at least icmpv6
void l4_ipv6_icmpv6(struct intnode *intn, struct ip6_hdr *iph, const uint16_t len, struct icmp6_hdr *icmpv6, const uint16_t plen)
{
	uint16_t		csum;
	struct grpintnode	*grpint;

	// Increase ICMP received statistics
	g_conf->stat_icmp_received++;
	intn->stat_icmp_received++;

	// We are only interrested in these types
	// Saves on calculating a checksum and then ignoring it anyways
	if (	icmpv6->icmp6_type != ICMP6_MEMBERSHIP_REPORT &&
		icmpv6->icmp6_type != ICMP6_MEMBERSHIP_REDUCTION &&
		icmpv6->icmp6_type != ICMP6_V2_MEMBERSHIP_REPORT &&
		icmpv6->icmp6_type != ICMP6_MEMBERSHIP_QUERY &&
		icmpv6->icmp6_type != ICMP6_ECHO_REQUEST)
	{
		D(dolog(LOG_DEBUG, "Ignoring ICMPv6 typed %d\n", icmpv6->icmp6_type);)
		return;
	}

	// Save the checksum
	csum = icmpv6->icmp6_cksum;
	// Clear it temporarily
	icmpv6->icmp6_cksum = 0;

	// Verify checksum
	icmpv6->icmp6_cksum = ipv6_checksum(iph, IPPROTO_ICMPV6, (uint8_t *)icmpv6, plen);
	if (icmpv6->icmp6_cksum != csum)
	{
		dolog(LOG_WARNING, "CORRUPT->DROP (%s): Received a ICMPv6 %s (%d) with wrong checksum (%x vs %x)\n",
			intn->name,
			 icmpv6->icmp6_type == ICMP6_MEMBERSHIP_REPORT		? "MLDv1 Membership Report" :
			(icmpv6->icmp6_type == ICMP6_MEMBERSHIP_REDUCTION	? "MLDv1 Membership Reduction" :
			(icmpv6->icmp6_type == ICMP6_V2_MEMBERSHIP_REPORT	? "MLDv2 Membership Report" : 
			(icmpv6->icmp6_type == ICMP6_MEMBERSHIP_QUERY		? "MLDv1/2 Membership Query" :
			(icmpv6->icmp6_type == ICMP6_ECHO_REQUEST		? "ICMPv6 Echo Request" :
										  "Unknown")))),
			icmpv6->icmp6_type,
			icmpv6->icmp6_cksum, csum);
	}

	if (icmpv6->icmp6_type == ICMP6_ECHO_REQUEST)
	{
		// We redistribute IPv6 ICMPv6 Echo Requests to the subscribers
		// This allows hosts to ping a IPv6 Multicast address and see who is listening ;)

		// Decrease the hoplimit, but only if not 0 yet
		if (iph->ip6_hlim > 0) iph->ip6_hlim--;
		D(else dolog(LOG_DEBUG, "Hoplimit for ICMPv6 packet was already %d\n", iph->ip6_hlim);)
		if (iph->ip6_hlim == 0)
		{
			g_conf->stat_hlim_exceeded++;
			// Send a time_exceed_transit error
			icmp6_send(intn, &iph->ip6_src, ICMP6_ECHO_REPLY, ICMP6_TIME_EXCEED_TRANSIT, &icmpv6->icmp6_data32, plen-sizeof(*icmpv6)+sizeof(icmpv6->icmp6_data32));
			return;
		}
		// Send this packet along it's way
		else l4_ipv6_multicast(intn, iph, len);
	}
	else
	{
		if (!(IN6_IS_ADDR_LINKLOCAL(&iph->ip6_src)))
		{
			char addr[INET6_ADDRSTRLEN];
			memset(addr,0,sizeof(addr));
			inet_ntop(AF_INET6, &iph->ip6_src, addr, sizeof(addr));
			dolog(LOG_WARNING, "Ignoring non-LinkLocal MLD from %s received from %s\n", addr, intn->name);
			return;
		}

		if (icmpv6->icmp6_type == ICMP6_MEMBERSHIP_REPORT)
		{
			l4_ipv6_icmpv6_mld1_report(intn, iph, len, (struct mld1 *)icmpv6, plen);
		}
		else if (icmpv6->icmp6_type == ICMP6_MEMBERSHIP_REDUCTION)
		{
			l4_ipv6_icmpv6_mld1_reduction(intn, iph, len, (struct mld1 *)icmpv6, plen);
		}
	// We simply ignore it if we don't support it ;)
	#ifdef ECMH_SUPPORT_MLD2
		else if (icmpv6->icmp6_type == ICMP6_V2_MEMBERSHIP_REPORT)
		{
			l4_ipv6_icmpv6_mld2_report(intn, iph, len, (struct mld2_report *)icmpv6, plen);
		}
	#endif // ECMH_SUPPORT_MLD2
		else if (icmpv6->icmp6_type == ICMP6_MEMBERSHIP_QUERY)
		{
			l4_ipv6_icmpv6_mld_query(intn, iph, len, (struct mld2_query *)icmpv6, plen);
		}
		D(else dolog(LOG_DEBUG, "ICMP type %d got through\n", icmpv6->icmp6_type);)
	}
	return;
}

void l3_ipv6(struct intnode *intn, struct ip6_hdr *iph, const uint16_t len)
{
	struct ip6_ext		*ipe;
	uint8_t			ipe_type;
	uint16_t		plen;
	uint32_t		l;

	struct groupnode	*groupn;
	struct grpintnode	*grpintn;
	struct subscrnode	*subscrn;
	struct listnode		*in, *in2;

	// Destination must be multicast
	// We don't care about unicast destinations
	// Those are handled by the OS itself.
	if (!IN6_IS_ADDR_MULTICAST(&iph->ip6_dst))
	{
		return;
	}

	// Save the type of the next header
	ipe_type = iph->ip6_nxt;
	// Step to the next header
	ipe = (struct ip6_ext *)(((void *)iph) + sizeof(*iph));
	plen = ntohs(iph->ip6_plen);

	// Skip the headers that we know
	while (	ipe_type == IPPROTO_HOPOPTS ||
		ipe_type == IPPROTO_ROUTING ||
		ipe_type == IPPROTO_DSTOPTS ||
		ipe_type == IPPROTO_AH)
	{
		// Save the type of the next header
		ipe_type = ipe->ip6e_nxt;

		// Step to the next header
		l = ((ipe->ip6e_len*8)+8);
		plen -= l;
		ipe  = (struct ip6_ext *)(((void *)ipe) + l);

		// Check for corrupt packets
		if ((void *)ipe > (((void *)iph)+len))
		{
			dolog(LOG_WARNING, "CORRUPT->DROP (%s): Header chain beyond packet data\n", intn->name);
			return;
		}
	}

	// Check for ICMP
	if (ipe_type == IPPROTO_ICMPV6)
	{
		// Take care of ICMPv6
		l4_ipv6_icmpv6(intn, iph, len, (struct icmp6_hdr *)ipe, plen);
		return;
	}

	// Handle multicast packets
	if (IN6_IS_ADDR_MULTICAST(&iph->ip6_dst))
	{
		// Decrease the hoplimit, but only if not 0 yet
		if (iph->ip6_hlim > 0) iph->ip6_hlim--;
		D(else dolog(LOG_DEBUG, "Hoplimit for UDP packet was already %d\n", iph->ip6_hlim);)
		if (iph->ip6_hlim == 0)
		{
			g_conf->stat_hlim_exceeded++;
			return;
		}

		l4_ipv6_multicast(intn, iph, len);
		return;
	}

	// Ignore the rest
	return;
}

void l2(struct intnode *intn, const uint8_t *packet, const uint16_t len, u_int16_t ether_type)
{
#ifdef ECMH_SUPPORT_IPV4
	if (ether_type == ETHERTYPE_IP)
	{
		l3_ipv4(intn, packet, len);
		return;
	}
	else
#endif // ECMH_SUPPORT_IPV4
	if (ether_type == ETH_P_IPV6)
	{
		l3_ipv6(intn, (struct ip6_hdr *)packet, len);
		return;
	}
	
	// We don't care about anything else...
	return;
}

// Initiliaze interfaces 
void update_interfaces(struct intnode *intn)
{
	FILE		*file;
	char		buf[100], devname[IFNAMSIZ];
	struct in6_addr	addr;
	int		ifindex = 0, prefixlen, scope, flags;
	int		gotlinkl = false, gotglobal = false;

	D(dolog(LOG_DEBUG, "*** Updating Interfaces\n");)

	// Get link local addresses from /proc/net/if_inet6
	file = fopen("/proc/net/if_inet6", "r");

	// We can live without it though
	if (!file)
	{
		dolog(LOG_WARNING, "Couldn't open /proc/net/if_inet6 for figuring out local interfaces\n");
		return;
	}

	// Format "fe80000000000000029027fffe24bbab 02 0a 20 80     eth0"
	while (fgets(buf, sizeof(buf), file))
	{
		if (21 != sscanf( buf,
			"%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx %x %x %x %x %8s",
			&addr.s6_addr[ 0], &addr.s6_addr[ 1], &addr.s6_addr[ 2], &addr.s6_addr[ 3],
			&addr.s6_addr[ 4], &addr.s6_addr[ 5], &addr.s6_addr[ 6], &addr.s6_addr[ 7],
			&addr.s6_addr[ 8], &addr.s6_addr[ 9], &addr.s6_addr[10], &addr.s6_addr[11],
			&addr.s6_addr[12], &addr.s6_addr[13], &addr.s6_addr[14], &addr.s6_addr[15],
			&ifindex, &prefixlen, &scope, &flags, devname))
		{
			dolog(LOG_WARNING, "/proc/net/if_inet6 has a broken line, ignoring");
			continue;
		}

		// Skip everything we don't care about
		if (	!IN6_IS_ADDR_LINKLOCAL(&addr) &&
			(
				IN6_IS_ADDR_UNSPECIFIED(&addr) ||
				IN6_IS_ADDR_LOOPBACK(&addr) ||
				IN6_IS_ADDR_MULTICAST(&addr))
			)
		{
			D(
				char txt[INET6_ADDRSTRLEN];
				memset(txt,0,sizeof(txt));
				inet_ntop(AF_INET6, &addr, txt, sizeof(txt));
				dolog(LOG_DEBUG, "Ignoring other address %s on interface %s\n", txt, devname);
			)
			continue;
		}

		if (intn)
		{
			// Was this the one to update?
			if (intn->ifindex == ifindex)
			{
				if (IN6_IS_ADDR_LINKLOCAL(&addr))
				{
					// Update the linklocal address
					memcpy(&intn->linklocal, &addr, sizeof(intn->linklocal));
					gotlinkl = true;
				}
				else
				{
					// Update the global address
					memcpy(&intn->global, &addr, sizeof(intn->global));
					gotglobal = true;
				}
				// We are done updating
				if (gotlinkl && gotglobal) break;
			}
		}
		// Update everything
		else
		{
			int newintn = false;
			gotlinkl = gotglobal = false;

			intn = int_find(ifindex, false);
			if (!intn)
			{
				intn = int_create(ifindex);
				newintn = true;
			}

			if (intn)
			{
				if (IN6_IS_ADDR_LINKLOCAL(&addr))
				{
					// Update the linklocal address
					memcpy(&intn->linklocal, &addr, sizeof(intn->linklocal));
					gotlinkl = true;
				}
				else
				{
					// Update the global address
					memcpy(&intn->global, &addr, sizeof(intn->global));
					gotglobal = true;
				}
			}

			// Add it to the list if it a new one and
			// either the linklocal or global was set.
			if (newintn)
			{
				if (gotlinkl || gotglobal) int_add(intn);
				else int_destroy(intn);
			}

			// We where not searching for a specific interface
			// thus clear this out
			intn = NULL;
		}
	}
	D(dolog(LOG_DEBUG, "*** Updating Interfaces - done\n");)
	fclose(file);
}

void init()
{
	g_conf = malloc(sizeof(struct conf));
	if (!g_conf)
	{
		dolog(LOG_ERR, "Couldn't init()\n");
		exit(-1);
	}

	// Clear it, never bad, always good 
	memset(g_conf, 0, sizeof(*g_conf));

	// Initialize our configuration
	g_conf->maxgroups	= 42;				// FIXME: Not verified yet...
	g_conf->daemonize	= true;

	// Initialize our list of interfaces
	g_conf->ints = list_new();
	g_conf->ints->del = (void(*)(void *))int_destroy;

	// Initialize our list of groups
	g_conf->groups = list_new();
	g_conf->groups->del = (void(*)(void *))group_destroy;

	// Initialize our counters
	g_conf->stat_starttime		= time(NULL);
	g_conf->stat_packets_received	= 0;
	g_conf->stat_packets_sent	= 0;
	g_conf->stat_bytes_received	= 0;
	g_conf->stat_bytes_sent		= 0;
	g_conf->stat_icmp_received	= 0;
	g_conf->stat_icmp_sent		= 0;
	g_conf->stat_hlim_exceeded	= 0;
}

void sighup(int i)
{
	// Reset the signal
	signal(SIGHUP, &sighup);
}

// Dump the statistical information
void sigusr1(int i)
{
	struct intnode		*intn;
	struct groupnode	*groupn;
	struct listnode		*ln;
	struct grpintnode	*grpintn;
	struct listnode		*gn;
	struct subscrnode	*subscrn;
	struct listnode		*ssn;
	time_t			time_tee;
	char			addr[INET6_ADDRSTRLEN];
	int			subscriptions = 0;
	int			uptime, uptime_s, uptime_m, uptime_h, uptime_d;
	FILE			*out;

	// Get the current time
	time_tee  = time(NULL);
	uptime_s  = time_tee - g_conf->stat_starttime;
	uptime_d  = uptime_s / (24*60*60);
	uptime_s -= uptime_d *  24*60*60;
	uptime_h  = uptime_s / (60*60);
	uptime_s -= uptime_h *  60*60;
	uptime_m  = uptime_s /  60;
	uptime_s -= uptime_m *  60;

	// Rewind the file to the start
	rewind(g_conf->stat_file);

	// Truncate the file
	ftruncate(fileno(g_conf->stat_file), (off_t)0);

	// Dump out all the groups with their information
	fprintf(g_conf->stat_file, "*** Subscription Information Dump\n");

	LIST_LOOP(g_conf->groups, groupn, ln)
	{
		inet_ntop(AF_INET6, &groupn->mca, addr, sizeof(addr));
		fprintf(g_conf->stat_file, "Group: %s\n", addr);

		LIST_LOOP(groupn->interfaces, grpintn, gn)
		{
			fprintf(g_conf->stat_file, "\tInterface: %s\n", grpintn->interface->name);

			LIST_LOOP(grpintn->subscriptions, subscrn, ssn)
			{
				int i = time_tee - subscrn->refreshtime;
				if (i < 0) i = -i;

				inet_ntop(AF_INET6, &subscrn->ipv6, addr, sizeof(addr));
				fprintf(g_conf->stat_file, "\t\t%s (%d seconds old)\n", addr, i);

				subscriptions++;
			}
		}
	}

	fprintf(g_conf->stat_file, "*** Subscription Information Dump (end - %d groups, %d subscriptions)\n", g_conf->groups->count, subscriptions);
	fprintf(g_conf->stat_file, "\n");

	// Dump all the interfaces
	fprintf(g_conf->stat_file, "*** Interface Dump\n");

	LIST_LOOP(g_conf->ints, intn, ln)
	{
		fprintf(g_conf->stat_file, "\n");
		fprintf(g_conf->stat_file, "Interface: %s\n", intn->name);
		fprintf(g_conf->stat_file, "  Index number           : %d\n", intn->ifindex);
		fprintf(g_conf->stat_file, "  MTU                    : %d\n", intn->mtu);

		inet_ntop(AF_INET6, &intn->linklocal, addr, sizeof(addr));
		fprintf(g_conf->stat_file, "  Link-local address     : %s\n", addr);

		inet_ntop(AF_INET6, &intn->global, addr, sizeof(addr));
		fprintf(g_conf->stat_file, "  Global unicast address : %s\n", addr);

		if (intn->mld_version == 0)
		fprintf(g_conf->stat_file, "  MLD version            : none\n");
		else
		fprintf(g_conf->stat_file, "  MLD version            : v%d\n", intn->mld_version);

		fprintf(g_conf->stat_file, "  Packets received       : %lld\n", intn->stat_packets_received);
		fprintf(g_conf->stat_file, "  Packets sent           : %lld\n", intn->stat_packets_sent);
		fprintf(g_conf->stat_file, "  Bytes received         : %lld\n", intn->stat_bytes_received);
		fprintf(g_conf->stat_file, "  Bytes sent             : %lld\n", intn->stat_bytes_sent);
		fprintf(g_conf->stat_file, "  ICMP's received        : %lld\n", intn->stat_icmp_received);
		fprintf(g_conf->stat_file, "  ICMP's sent            : %lld\n", intn->stat_icmp_sent);
	}

	fprintf(g_conf->stat_file, "\n");
	fprintf(g_conf->stat_file, "*** Interface Dump (end - %d interfaces)\n", g_conf->ints->count);
	fprintf(g_conf->stat_file, "\n");


	// Dump out some generic program statistics
	strftime(addr, sizeof(addr), "%Y-%m-%d %T", gmtime(&g_conf->stat_starttime));

	fprintf(g_conf->stat_file, "*** Statistics Dump\n");
	fprintf(g_conf->stat_file, "Version              : ecmh %s\n", ECMH_VERSION);
	fprintf(g_conf->stat_file, "Started              : %s GMT\n", addr);
	fprintf(g_conf->stat_file, "Uptime               : %d days %02d:%02d:%02d\n", uptime_d, uptime_h, uptime_m, uptime_s);
	fprintf(g_conf->stat_file, "Interfaces Monitored : %d\n", g_conf->ints->count);
	fprintf(g_conf->stat_file, "Groups Managed       : %d\n", g_conf->groups->count);
	fprintf(g_conf->stat_file, "Total Subscriptions  : %d\n", subscriptions);
	fprintf(g_conf->stat_file, "Packets Received     : %lld\n", g_conf->stat_packets_received);
	fprintf(g_conf->stat_file, "Packets Sent         : %lld\n", g_conf->stat_packets_sent);
	fprintf(g_conf->stat_file, "Bytes Received       : %lld\n", g_conf->stat_bytes_received);
	fprintf(g_conf->stat_file, "Bytes Sent           : %lld\n", g_conf->stat_bytes_sent);
	fprintf(g_conf->stat_file, "ICMP's received      : %lld\n", g_conf->stat_icmp_received);
	fprintf(g_conf->stat_file, "ICMP's sent          : %lld\n", g_conf->stat_icmp_sent);
	fprintf(g_conf->stat_file, "Hop Limit Exceeded   : %lld\n", g_conf->stat_hlim_exceeded);
	fprintf(g_conf->stat_file, "*** Statistics Dump (end)\n");

	// Flush the information to disk
	fflush(g_conf->stat_file);

	dolog(LOG_INFO, "Dumped statistics into %s\n", ECMH_DUMPFILE);

	// Reset the signal
	signal(SIGUSR1, &sigusr1);
}

// Let's tell everybody we are a querier and ask
// them which groups they want to receive.
void send_mld_querys()
{
	struct intnode		*intn;
	struct listnode		*ln;
	struct in6_addr		any;

	D(dolog(LOG_DEBUG, "*** Sending MLD Queries\n");)

	// We want to know about all the groups
	memset(&any,0,sizeof(any));

	// Send MLD query's
	LIST_LOOP(g_conf->ints, intn, ln)
	{
		mld_send_query(intn, &any);
	}

	D(dolog(LOG_DEBUG, "*** Sending MLD Queries - done\n");)
}

void timeout_signal()
{
	// Mark it to be ignored, this avoids double timeouts
	// one never knows if it takes too long to handle
	// the first one.
	signal(SIGALRM, SIG_IGN);
	
	// Set the needs_timeout
	g_needs_timeout = true;
}

void timeout()
{
	struct groupnode	*groupn;
	struct listnode		*ln, *ln2;
	struct grpintnode	*grpintn;
	struct listnode		*gn, *gn2;
	struct subscrnode	*subscrn;
	struct listnode		*ssn, *ssn2;
	time_t			time_tee;

	D(dolog(LOG_DEBUG, "*** Timeout\n");)

	// Update the complete interfaces list
	update_interfaces(NULL);

	// Get the current time
	time_tee = time(NULL);

	// Timeout all the groups that didn't refresh yet
	LIST_LOOP2(g_conf->groups, groupn, ln, ln2)
	{
		LIST_LOOP2(groupn->interfaces, grpintn, gn, gn2)
		{
			LIST_LOOP2(grpintn->subscriptions, subscrn, ssn, ssn2)
			{
				// Calculate the difference
				int i = time_tee - subscrn->refreshtime;
				if (i < 0) i = -i;
				
				// Dead too long?
				if (i > (ECMH_SUBSCRIPTION_TIMEOUT * ECMH_ROBUSTNESS_FACTOR))
				{
					// Dead too long -> delete it
					list_delete_node(grpintn->subscriptions, ssn);
					// Destroy the subscription itself
					subscr_destroy(subscrn);
				}
			}
			LIST_LOOP2_END
		
			if (grpintn->subscriptions->count == 0)
			{
				// Delete from the list
				list_delete_node(groupn->interfaces, gn);
				// Destroy the grpint
				grpint_destroy(grpintn);
			}
		}
		LIST_LOOP2_END

		if (groupn->interfaces->count == 0)
		{
			// Delete from the list
			list_delete_node(g_conf->groups, ln);
			// Destroy the group
			group_destroy(groupn);
		}
	}
	LIST_LOOP2_END

	// Send out MLD queries
	send_mld_querys();

	D(dolog(LOG_DEBUG, "*** Timeout - done\n");)
}

// Long options

static struct option const long_options[] = {
	{"foreground",	no_argument,		NULL, 'f'},
	{"user",	required_argument,	NULL, 'u'},
	{"group",	required_argument,	NULL, 'g'},
	{NULL,		0, NULL, 0},
};

int main(int argc, char *argv[], char *envp[])
{
	int			i=0, len;

	char			buffer[8192];
	struct sockaddr_ll	sa;
	socklen_t		salen;
	struct ether_header	*hdr_ether = (struct ether_header *)&buffer;
	struct ip		*hdr_ip = (struct ip *)&buffer;

	struct intnode		*intn = NULL;

	int			drop_uid = 0, drop_gid = 0, option_index = 0;
	struct passwd		*passwd;

	init();

	// Handle arguments
	while ((i = getopt_long(argc, argv, "fu:g:", long_options, &option_index)) != EOF)
	{
		switch (i)
		{
		case 0:
			// Long option
			break;

		case 'f':
			g_conf->daemonize = false;
			break;

		case 'u':
			passwd = getpwnam(optarg);
			if (passwd)
			{
				drop_uid = passwd->pw_uid;
				drop_gid = passwd->pw_gid;
			}
			else fprintf(stderr, "Couldn't find user %s, aborting\n", optarg);
			break;

		default:
			fprintf(stderr,
				"%s [-f] [-u username] [-g groupname]\n"
				"\n"
				"-f, --foreground          don't daemonize\n"
				"-u, --user username       drop (setuid+setgid) to user after startup\n"
				"\n"
				"Report bugs to Jeroen Massar <jeroen@unfix.org>.\n"
				"Also see the website at http://unfix.org/projects/ecmh/\n",
				argv[0]);
			return -1;
		}
	}

	// Daemonize
	if (g_conf->daemonize)
	{
		int i = fork();
		if (i < 0)
		{
			fprintf(stderr, "Couldn't fork\n");
			return -1;
		}
		// Exit the mother fork
		if (i != 0) return 0;

		// Child fork
		setsid();
		// Cleanup stdin/out/err
		freopen("/dev/null","r",stdin);
		freopen("/dev/null","w",stdout);
		freopen("/dev/null","w",stderr);
	}

	// Handle a SIGHUP to reload the config
	signal(SIGHUP, &sighup);

	// Handle SIGTERM/INT/KILL to cleanup the pid file and exit
	signal(SIGTERM,	&cleanpid);
	signal(SIGINT,	&cleanpid);
	signal(SIGKILL,	&cleanpid);

	// Timeout handling
	signal(SIGALRM, &timeout_signal);
	alarm(ECMH_SUBSCRIPTION_TIMEOUT);

	// Dump operations
	signal(SIGUSR1,	&sigusr1);

	signal(SIGUSR2, SIG_IGN);

	// Show our version in the startup logs ;)
	dolog(LOG_INFO, "Easy Cast du Multi Hub (ecmh) %s by Jeroen Massar <jeroen@unfix.org>\n", ECMH_VERSION);

	// Save our PID
	savepid();

	// Open our dump file
	g_conf->stat_file = fopen(ECMH_DUMPFILE, "w");
	if (!g_conf->stat_file)
	{
		dolog(LOG_ERR, "Couldn't open dumpfile %s\n", ECMH_DUMPFILE);
		return -1;
	}

	// Allocate a PACKET socket which can send and receive
	//  anything we want (anything ???.... anythinggg... ;)
	g_conf->rawsocket = socket(PF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
	if (g_conf->rawsocket < 0)
	{
		dolog(LOG_ERR, "Couldn't allocate a RAW socket\n");
		return -1;
	}

	// Fix our priority, we need to be near realtime
	if (setpriority(PRIO_PROCESS, getpid(), -15) == -1)
	{
		dolog(LOG_WARNING, "Couldn't raise priority to -15, if streams are shaky, upgrade your cpu or fix this\n");
	}

	// Drop our root priveleges.
	// We don't need them anymore anyways
	if (drop_uid != 0) setuid(drop_uid);
	if (drop_gid != 0) setgid(drop_gid);

	// Update the complete interfaces list
	update_interfaces(NULL);

	send_mld_querys();

	len = 0;
	while (len != -1)
	{
		// Was a timeout set?
		if (g_needs_timeout)
		{
			// Run timeout routine
			timeout();
			
			// Turn it off
			g_needs_timeout = false;

			// Reset the alarm
			signal(SIGALRM, &timeout);
			alarm(ECMH_SUBSCRIPTION_TIMEOUT);
		}
		
		salen = sizeof(sa);
		memset(&sa, 0, sizeof(sa));
		len = recvfrom(g_conf->rawsocket, &buffer, sizeof(buffer), 0, (struct sockaddr *)&sa, &salen);
		
		if (len == -1) break;

		// Ignore:
		// - loopback traffic
		// - any packets that originate from this host
		if (	sa.sll_hatype == ARPHRD_LOOPBACK ||
			sa.sll_pkttype == PACKET_OUTGOING) continue;

		// Update statistics
		g_conf->stat_packets_received++;
		g_conf->stat_bytes_received+=len;

		intn = int_find(sa.sll_ifindex, true);
		if (!intn)
		{
			// Create a new interface
			intn = int_create(sa.sll_ifindex);
			if (intn)
			{
				// Determine linklocal address etc.
				update_interfaces(intn);

				// Add it to the list
				int_add(intn);
			}
		}

		if (!intn)
		{
			dolog(LOG_ERR, "Couldn't find nor add interface link %d\n", sa.sll_ifindex);
			break;
		}

		// Update statistics
		intn->stat_packets_received++;
		intn->stat_bytes_received+=len;

		// Directly has a IP header so pretend it is IPv4
		// and use that header to find out the real version
#ifdef ECMH_SUPPORT_IPV4	
		if (hdr_ip->ip_v == 4) l3_ipv4(intn, buffer, len);
		else
#endif //ECMH_SUPPORT_IPV4
		if (hdr_ip->ip_v == 6) l3_ipv6(intn, (struct ip6_hdr *)buffer, len);

		// Ignore the rest
//		D(else dolog(LOG_DEBUG, "%5s Unknown IP version %d\n", intn->name, hdr_ip->ip_v);)
		fflush(stdout);

	}

	// Dump the stats one last time
	sigusr1(SIGUSR1);

	// Show the message in the log
	dolog(LOG_INFO, "Shutdown, thank you for using ecmh\n");

	// Cleanup the nodes
	list_delete_all_node(g_conf->ints);
	list_delete_all_node(g_conf->groups);

	// Close files and sockets
	fclose(g_conf->stat_file);
	close(g_conf->rawsocket);

	// Free the config memory
	free(g_conf);

	cleanpid(SIGINT);

	return 0;
}
