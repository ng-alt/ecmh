/**************************************
 ecmh - Easy Cast du Multi Hub
 by Jeroen Massar <jeroen@unfix.org>
***************************************
 $Author: fuzzel $
 $Id: ecmh.c,v 1.13 2004/10/08 13:45:33 fuzzel Exp $
 $Date: 2004/10/08 13:45:33 $
***************************************
 
   Docs:
 	netdevice(7), packet(7)
 	RFC 2710 - Multicast Listener Discovery (MLD) for IPv6
 	RFC 3569 - An Overview of Source-Specific Multicast (SSM)
 	RFC 3590 - Source Address Selection for the Multicast Listener Discovery (MLD) Protocol
 	RFC 3678 - Socket Interface Extensions for Multicast Source Filters
	RFC 3810 - Multicast Listener Discovery Version 2 (MLDv2) for IPv6

 	http://www.ietf.org/internet-drafts/draft-holbrook-idmr-igmpv3-ssm-05.txt
 		Using IGMPv3 and MLDv2 For Source-Specific Multicast

   - Querier Election support (MLDv2 7.6.2 + 7.1)
      - Not implemented otherwise ECMH won't work.

   Todo:
   - Protocol Robustness, send twice, first with S flag, second without.
   - Force check for HopByHop
 
***************************************/

#include "ecmh.h"

/* Configuration Variables */
struct conf	*g_conf;
volatile int	g_needs_timeout = false;

/* Prototypes, to forward some functions */
void update_interfaces(struct intnode *intn);
void l2_ethtype(struct intnode *intn, const uint8_t *packet, const unsigned int len, const unsigned int ether_type);
void l2_eth(struct intnode *intn, struct ether_header *eth, const unsigned int len);

/**************************************
  Functions
**************************************/

uint16_t inchksum(const void *data, uint32_t length)
{
	register long		sum = 0;
	register const uint16_t *wrd = (const uint16_t *)data;
	register long		slen = (long)length;

	while (slen >= 2)
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

	/* IPv6 Source + Dest */
	chksum  = inchksum(&ip6->ip6_src, sizeof(ip6->ip6_src) + sizeof(ip6->ip6_dst));
	chksum += inchksum(&pseudo, sizeof(pseudo));
	chksum += inchksum(data, length);

	/* Wrap in the carries to reduce chksum to 16 bits. */
	chksum  = (chksum >> 16) + (chksum & 0xffff);
	chksum += (chksum >> 16);

	/* Take ones-complement and replace 0 with 0xFFFF. */
	chksum = (uint16_t) ~chksum;
	if (chksum == 0UL) chksum = 0xffffUL;
	return (uint16_t)chksum;
}

struct lookup
{
	unsigned int	num;
	const char	*desc;
} icmpv6_types[] = {
	{ ICMP6_DST_UNREACH,			"Destination Unreachable"		},
	{ ICMP6_PACKET_TOO_BIG,			"Packet too big"			},
	{ ICMP6_TIME_EXCEEDED,			"Time Exceeded"				},
	{ ICMP6_PARAM_PROB,			"Parameter Problem"			},
	{ ICMP6_ECHO_REQUEST,			"Echo Request"				},
	{ ICMP6_ECHO_REPLY,			"Echo Reply"				},
	{ ICMP6_MEMBERSHIP_QUERY,		"Membership Query"			},
	{ ICMP6_MEMBERSHIP_REPORT,		"Membership Report"			},
	{ ICMP6_MEMBERSHIP_REDUCTION,		"Membership Reduction"			},
	{ ICMP6_V2_MEMBERSHIP_REPORT,		"Membership Report (V2)"		},
	{ ICMP6_V2_MEMBERSHIP_REPORT_EXP,	"Membership Report (V2) - Experimental"	},
	{ ND_ROUTER_SOLICIT,			"ND Router Solicitation"		},
	{ ND_ROUTER_ADVERT,			"ND Router Advertisement"		},
	{ ND_NEIGHBOR_SOLICIT,			"ND Neighbour Solicitation"		},
	{ ND_NEIGHBOR_ADVERT,			"ND Neighbour Advertisement"		},
	{ ND_REDIRECT,				"ND Redirect"				},
	{ ICMP6_ROUTER_RENUMBERING,		"Router Renumbering",			},
	{ ICMP6_NI_QUERY,			"Node Information Query"		},
	{ ICMP6_NI_REPLY,			"Node Information Reply"		},
	{ MLD_MTRACE_RESP,			"Mtrace Response"			},
	{ MLD_MTRACE,				"Mtrace Message"			},
	{ 0,					NULL },
}, icmpv6_codes_unreach[] = {
	{ ICMP6_DST_UNREACH_NOROUTE,		"No route to destination"		},
	{ ICMP6_DST_UNREACH_ADMIN,		"Administratively prohibited"		},
	{ ICMP6_DST_UNREACH_NOTNEIGHBOR,	"Not a neighbor (obsolete)"		},
	{ ICMP6_DST_UNREACH_BEYONDSCOPE,	"Beyond scope of source address"	},
	{ ICMP6_DST_UNREACH_ADDR,		"Address Unreachable"			},
	{ ICMP6_DST_UNREACH_NOPORT,		"Port Unreachable"			},
}, icmpv6_codes_ttl[] = {
	{ ICMP6_TIME_EXCEED_TRANSIT,		"Time Exceeded during Transit",		},
	{ ICMP6_TIME_EXCEED_REASSEMBLY,		"Time Exceeded during Reassembly"	},
}, icmpv6_codes_param[] = {
	{ ICMP6_PARAMPROB_HEADER,		"Erroneous Header Field"		},
	{ ICMP6_PARAMPROB_NEXTHEADER,		"Unrecognized Next Header"		},
	{ ICMP6_PARAMPROB_OPTION,		"Unrecognized Option"			},
}, icmpv6_codes_ni[] = {
	{ ICMP6_NI_SUCCESS,			"Node Information Successful Reply"	},
	{ ICMP6_NI_REFUSED,			"Node Information Request Is Refused"	},
	{ ICMP6_NI_UNKNOWN,			"Unknown Qtype"				},
}, icmpv6_codes_renumber[] = {
	{ ICMP6_ROUTER_RENUMBERING_COMMAND,	"Router Renumbering Command"		},
	{ ICMP6_ROUTER_RENUMBERING_RESULT,	"Router Renumbering Result"		},
	{ ICMP6_ROUTER_RENUMBERING_SEQNUM_RESET,"Router Renumbering Sequence Number Reset"},
}, mld2_grec_types[] = {
	{ MLD2_MODE_IS_INCLUDE,			"MLDv2 Mode Is Include" 		},
	{ MLD2_MODE_IS_EXCLUDE,			"MLDv2 Mode Is Exclude"			},
	{ MLD2_CHANGE_TO_INCLUDE,		"MLDv2 Change to Include"		},
	{ MLD2_CHANGE_TO_EXCLUDE,		"MLDv2 Change to Exclude"		},
	{ MLD2_ALLOW_NEW_SOURCES,		"MLDv2 Allow New Source"		},
	{ MLD2_BLOCK_OLD_SOURCES,		"MLDv2 Block Old Sources"		},
};

const char *lookup(struct lookup *l, unsigned int num)
{
	unsigned int i;
	for (i=0; l && l[i].desc; i++)
	{
		if (l[i].num != num) continue;
		return l[i].desc;
	}
	return "Unknown";
}

#define icmpv6_type(type) lookup(icmpv6_types, type)

const char *icmpv6_code(unsigned int type, unsigned int code)
{
	struct lookup *l = NULL;
	switch (type)
	{
		case ICMP6_DST_UNREACH:		l = icmpv6_codes_unreach;	break;
		case ICMP6_TIME_EXCEEDED:	l = icmpv6_codes_ttl;		break;
		case ICMP6_PARAM_PROB:		l = icmpv6_codes_param;		break;
		case ICMP6_NI_QUERY:
		case ICMP6_NI_REPLY:		l = icmpv6_codes_ni;		break;
		case ICMP6_ROUTER_RENUMBERING:	l = icmpv6_codes_renumber;	break;
	}
	return lookup(l, code);
}

/* Send a packet */
void sendpacket6(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len)
{
	int     sent;
#ifndef ECMH_BPF
	struct sockaddr_ll	sa;

	memset(&sa, 0, sizeof(sa));

	sa.sll_family	= AF_PACKET;
	sa.sll_protocol	= htons(ETH_P_IPV6);
	sa.sll_ifindex	= intn->ifindex;
	sa.sll_hatype	= intn->hwaddr.sa_family;
	sa.sll_pkttype	= 0;
	sa.sll_halen	= 6;

	/*
	 * Construct a Ethernet MAC address from the IPv6 destination multicast address.
	 * Per RFC2464
	 */
	sa.sll_addr[0] = 0x33;
	sa.sll_addr[1] = 0x33;
	sa.sll_addr[2] = iph->ip6_dst.s6_addr[12];
	sa.sll_addr[3] = iph->ip6_dst.s6_addr[13];
	sa.sll_addr[4] = iph->ip6_dst.s6_addr[14];
	sa.sll_addr[5] = iph->ip6_dst.s6_addr[15];

	/* Send the packet */
	errno = 0;
	sent = sendto(g_conf->rawsocket, iph, len, 0, (struct sockaddr *)&sa, sizeof(sa));

#else /* !ECMH_BPF */

	register uint32_t	chksum = 0;
	struct ether_header	hdr_eth;
	struct ip		hdr_ip;
	struct iovec		vector[3];

	/* There is always ethernet to send out */
	vector[0].iov_base	= &hdr_eth;
	vector[0].iov_len 	= sizeof(hdr_eth);

	/*
	 * Construct a Ethernet MAC address from the IPv6 destination multicast address.
	 * Per RFC2464
	 */
	memset(&hdr_eth, 0, sizeof(hdr_eth));
	hdr_eth.ether_dhost[0] = 0x33;
	hdr_eth.ether_dhost[1] = 0x33;
	hdr_eth.ether_dhost[2] = iph->ip6_dst.s6_addr[12];
	hdr_eth.ether_dhost[3] = iph->ip6_dst.s6_addr[13];
	hdr_eth.ether_dhost[4] = iph->ip6_dst.s6_addr[14];
	hdr_eth.ether_dhost[5] = iph->ip6_dst.s6_addr[15];

	/*
	 * Handle non-tunneledmode & native ethernet
	 */
	if (!g_conf->tunnelmode || !intn->master)
	{
		/* Send a Native IPv6 packet */
		hdr_eth.ether_type	= htons(ETH_P_IPV6);
		vector[1].iov_base	= (void *)iph;
		vector[1].iov_len 	= len;

		dolog(LOG_DEBUG, "Sending Native IPv6 packet over %s\n", intn->name);
		sent = writev(intn->socket, vector, 2);
	}

	/*
	 * When this interface is a tunnel, send it over it's parent socket
	 * After having it encapsulated in proto-41
	 */
	else
	{	
		/* Construct the proto-41 packet */
		memset(&hdr_ip, 0, sizeof(hdr_ip));
		hdr_ip.ip_v 	= 4;
		hdr_ip.ip_hl	= 5;
		hdr_ip.ip_tos	= 0;
		hdr_ip.ip_len	= htons(len + sizeof(hdr_ip));
		hdr_ip.ip_id	= htons(42);
		hdr_ip.ip_off	= 0;
		hdr_ip.ip_ttl	= 100;
		hdr_ip.ip_p	= IPPROTO_IPV6;
		hdr_ip.ip_sum	= 0;
		memcpy(&hdr_ip.ip_src, &intn->master->ipv4_local, sizeof(hdr_ip.ip_src));
		memcpy(&hdr_ip.ip_dst, &intn->ipv4_remote, sizeof(hdr_ip.ip_dst));

		/* Calculate the checksum */
		chksum = inchksum(&hdr_ip, sizeof(hdr_ip));

		/* Wrap in the carries to reduce chksum to 16 bits. */
		chksum  = (chksum >> 16) + (chksum & 0xffff);
		chksum += (chksum >> 16);

		/* Take ones-complement and replace 0 with 0xFFFF. */
		chksum = (uint16_t) ~chksum;
		if (chksum == 0UL) chksum = 0xffffUL;

		/* Fill in the Checksum */
		hdr_ip.ip_sum = (uint16_t)chksum;

		/* Send a IPv4 proto-41 packet over the master's socket */
		hdr_eth.ether_type	= htons(ETH_P_IP);
		vector[1].iov_base 	= &hdr_ip;
		vector[1].iov_len 	= sizeof(hdr_ip);
		vector[2].iov_base	= (void *)iph;
		vector[2].iov_len 	= len;

		dolog(LOG_DEBUG, "Sending proto-41 IPv6 packet for %s over %s\n", intn->name, intn->master->name);
		sent = writev(intn->master->socket, vector, 3);
	}
#endif /* !ECMH_BPF */
	if (sent < 0)
	{
		/*
		 * Remove the device if it doesn't exist anymore,
		 * can happen with dynamic tunnels etc
		 */
		if (errno == ENXIO)
		{
			/* Delete from the list */
			listnode_delete(g_conf->ints, intn);
			/* Destroy the interface itself */
			int_destroy(intn);
		}
		else dolog(LOG_DEBUG, "[%-5s] sending %u bytes failed, mtu = %u: %s (%d)\n", intn->name, len, intn->mtu, strerror(errno), errno);
		return;
	}

	/* Update the global statistics */
	g_conf->stat_packets_sent++;
	g_conf->stat_bytes_sent+=len;

	/* Update interface statistics */
	intn->stat_bytes_sent+=len;
	intn->stat_packets_sent++;
	return;
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

	/* Create the IPv6 packet */
	packet.ip6.ip6_vfc	= 0x60;
	packet.ip6.ip6_plen	= ntohs(sizeof(packet) -
				  (sizeof(packet.data) - dlen) -
				  sizeof(packet.icmp6.icmp6_data32) -
				  sizeof(packet.ip6));
	packet.ip6.ip6_nxt	= IPPROTO_ICMPV6;
	/* Hoplimit of 64 seems to be a semi default */
	packet.ip6.ip6_hlim	= 64;

	/*
	 * The source address must be a global unicast IPv6 address
	 * and should be associated to the interface we are sending on
	 */
	memcpy(&packet.ip6.ip6_src, &intn->global, sizeof(packet.ip6.ip6_src));

	/* Target == Sender */
	memcpy(&packet.ip6.ip6_dst, src, sizeof(*src));

	/* ICMPv6 Error Report */
	packet.icmp6.icmp6_type	= type;
	packet.icmp6.icmp6_code	= code;
	
	/* Add the data, we start at the data in the icmp6 packet */
	memcpy(&packet.icmp6.icmp6_data32, data, (sizeof(packet.data) > dlen ? dlen : sizeof(packet.data)));

	/* Calculate and fill in the checksum */
	packet.icmp6.icmp6_cksum	= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.icmp6, sizeof(packet.icmp6) + dlen - sizeof(packet.icmp6.icmp6_data32));

	dolog(LOG_DEBUG, "Sending ICMPv6 Type %s (%u) code %s (%u) on %s\n", icmpv6_type(type), type, icmpv6_code(type, code), code, intn->name);
	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet) - (sizeof(packet.data) - dlen) - sizeof(packet.icmp6.icmp6_data32));

	/* Increase ICMP sent statistics */
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}

/*
 * MLDv1 and MLDv2 are backward compatible when doing Queries
 * aka a router implementing MLDv2 can send the same query
 * and both MLDv1 and MLDv2 hosts will understand it.
 * MLDv2 hosts will return a MLDv2 report, MLDv1 hosts a MLDv1 report
 * ecmh will always send MLDv2 queries even though we might have
 * seen MLDv1's coming in.
 *
 * src specifies the Source IPv6 address, may be NULL to replace it with any
 */
#ifndef ECMH_SUPPORT_MLD2
void mld_send_query(struct intnode *intn, const struct in6_addr *mca, const struct in6_addr *src)
#else
void mld_send_query(struct intnode *intn, const struct in6_addr *mca, const struct in6_addr *src, bool suppression)
#endif
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
		struct in6_addr		src;
#else
		struct mld1		mldq;
#endif
		
	} packet;
	unsigned int	packetlen;

	memset(&packet, 0, sizeof(packet));

	/* Create the IPv6 packet */
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_nxt		= IPPROTO_HOPOPTS;
	packet.ip6.ip6_hlim		= 1;

	/*
	 * The source address must be the link-local address
	 * of the interface we are sending on
	 */
	memcpy(&packet.ip6.ip6_src, &intn->linklocal, sizeof(packet.ip6.ip6_src));

	/* Generaly Query -> link-scope all-nodes (ff02::1) */
	packet.ip6.ip6_dst.s6_addr[0]	= 0xff;
	packet.ip6.ip6_dst.s6_addr[1]	= 0x02;
	packet.ip6.ip6_dst.s6_addr[15]	= 0x01;

	/* HopByHop Header Extension */
	packet.hbh.ip6h_nxt		= IPPROTO_ICMPV6;
	packet.hbh.ip6h_len		= 0;
	
	/* Router Alert Option */
	packet.routeralert.type		= 5;
	packet.routeralert.length	= sizeof(packet.routeralert.value);
	packet.routeralert.value	= 0;			/* MLD ;) */

	/* Option Padding */
	packet.routeralert.optpad[0]	= IP6OPT_PADN;
	packet.routeralert.optpad[1]	= 0;

	/* ICMPv6 MLD Query */
	packet.mldq.type		= ICMP6_MEMBERSHIP_QUERY;
	packet.mldq.mrc			= htons(2000);

	/*
	 * The address to query, can be in6addr_any to
	 * query for everything or a specific group
	 */
	memcpy(&packet.mldq.mca, mca, sizeof(*mca));

#ifndef ECMH_SUPPORT_MLD2
	packetlen			= sizeof(packet);
#else
	if (src)
	{
		/* We specify a source IPv6 address */
		printf("With Source\n");
		packetlen		= sizeof(packet);
		packet.mldq.nsrcs	= htons(1);
		memcpy(&packet.src, src, sizeof(packet.src));
	}
	else
	{
		/* No sources given */
		printf("No Source\n");
		packetlen		= sizeof(packet) - sizeof(packet.src);
		packet.mldq.nsrcs	= htons(0);
	}
	packet.mldq.suppress		= suppression ? 1 : 0;
	packet.mldq.qrv			= ECMH_ROBUSTNESS_FACTOR;
	packet.mldq.qqic		= ECMH_SUBSCRIPTION_TIMEOUT;
#endif

	/* Calculate and fill in the checksum */
	packet.ip6.ip6_plen		= htons(packetlen - sizeof(packet.ip6));
	packet.mldq.csum		= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.mldq, packetlen - sizeof(packet.ip6) - sizeof(packet.hbh) - sizeof(packet.routeralert));

#ifndef ECMH_SUPPORT_MLD2
	dolog(LOG_DEBUG, "Sending MLDv1 Query on %s\n", intn->name);
#else
	dolog(LOG_DEBUG, "Sending MLDv2 Query on %s with %u sources\n", intn->name, ntohs(packet.mldq.nsrcs));
#endif
	sendpacket6(intn, (const struct ip6_hdr *)&packet, packetlen);

	/* Increase ICMP sent statistics */
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

	/* Create the IPv6 packet */
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_plen		= ntohs(sizeof(packet) - sizeof(packet.ip6));
	packet.ip6.ip6_nxt		= IPPROTO_HOPOPTS;
	packet.ip6.ip6_hlim		= 1;

	/*
	 * The source address must be the link-local address
	 * of the interface we are sending on
	 */
	memcpy(&packet.ip6.ip6_src, &intn->linklocal, sizeof(packet.ip6.ip6_src));

	/* Report -> Multicast address */
	memcpy(&packet.ip6.ip6_dst, mca, sizeof(*mca));

	/* HopByHop Header Extension */
	packet.hbh.ip6h_nxt		= IPPROTO_ICMPV6;
	packet.hbh.ip6h_len		= 0;

	/* Router Alert Option */
	packet.routeralert.type		= 5;
	packet.routeralert.length	= sizeof(packet.routeralert.value);
	packet.routeralert.value	= 0;			/* MLD ;) */

	/* Option Padding */
	packet.routeralert.optpad[0]	= IP6OPT_PADN;
	packet.routeralert.optpad[1]	= 0;

	/* ICMPv6 MLD Report */
	packet.mld1.type		= ICMP6_MEMBERSHIP_REPORT;
	packet.mld1.mrc			= 0;
	memcpy(&packet.mld1.mca, mca, sizeof(*mca));

	/* Calculate and fill in the checksum */
	packet.mld1.csum		= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.mld1, sizeof(packet.mld1));

	dolog(LOG_DEBUG, "Sending MLDv1 Report on %s\n", intn->name);
	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet));

	/* Increase ICMP sent statistics */
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}

#ifdef ECMH_SUPPORT_MLD2
void mld2_send_report(struct intnode *intn, const struct in6_addr *mca)
{
	struct groupnode		*groupn;
	struct grpintnode		*grpintn;
	struct listnode			*ln, *gn;
	unsigned int			this_srcs=0, groups=0, srcs=0, grec_added=0;
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

	/* Create the IPv6 packet */
	packet.ip6.ip6_vfc		= 0x60;
	packet.ip6.ip6_plen		= ntohs(sizeof(packet) - sizeof(packet.ip6));
	packet.ip6.ip6_nxt		= IPPROTO_HOPOPTS;
	packet.ip6.ip6_hlim		= 1;

	/*
	 * The source address must be the link-local address
	 * of the interface we are sending on
	 */
	memcpy(&packet.ip6.ip6_src, &intn->linklocal, sizeof(packet.ip6.ip6_src));

	/* MLDv2 Report -> All IPv6 Multicast Routers (ff02::16) */
	packet.ip6.ip6_dst.s6_addr[0]	= 0xff;
	packet.ip6.ip6_dst.s6_addr[1]	= 0x02;
	packet.ip6.ip6_dst.s6_addr[15]	= 0x16;

	/* HopByHop Header Extension */
	packet.hbh.ip6h_nxt		= IPPROTO_ICMPV6;
	packet.hbh.ip6h_len		= 0;

	/* Router Alert Option */
	packet.routeralert.type		= 5;
	packet.routeralert.length	= sizeof(packet.routeralert.value);
	packet.routeralert.value	= 0;			/* MLD ;) */

	/* Option Padding */
	packet.routeralert.optpad[0]	= IP6OPT_PADN;
	packet.routeralert.optpad[1]	= 0;

	/* ICMPv6 MLD Report */
	packet.mld1.type		= ICMP6_V2_MEMBERSHIP_REPORT;
	packet.mld1.mrc			= 0;
	memcpy(&packet.mld1.mca, mca, sizeof(*mca));

	/*
	 * NOTE, we need to watch the MTU size, as there
	 * will be more subscriptions + SSM combo's than
	 * the size of one MTU can fit -> split it when needed.
	 */
	
	/* Count the number of groups + sources to be send */
	LIST_LOOP(g_conf->groups, groupn, ln)
	{
		/*
		 * If we only need to send for this MCA
		 * don't count anything else
		 */
		if (	(mca && IN6_ARE_ADDR_EQUAL(mca, &groupn->mca)) ||
			groupn->interfaces->count == 0) continue;
		
		this_srcs = 0;

		LIST_LOOP(groupn->interfaces, grpintn, gn)
		{
			/* Skip the sending interface */
			if (grpintn->interface == intn) continue;

			/* Count the source subscriptions */
			this_srcs++;
		}
		
		if (this_srcs > 0)
		{
			groups++;
			srcs+=this_srcs;
		}
	}
	
	/* Allocate memory to let the above all fit */
	
	/* Copy the header in front of it */
		
	/* Loop again and append the sources + headers */
	LIST_LOOP(g_conf->groups, groupn, ln)
	{
		/*
		 * If we only need to send for this MCA
		 * don't count anything else
		 */
		if (	(mca && IN6_ARE_ADDR_EQUAL(mca, &groupn->mca)) ||
			groupn->interfaces->count == 0) continue;

		/* We haven't added a group record yet */
		grec_added = 0;

		LIST_LOOP(groupn->interfaces, grpintn, gn)
		{
			/* Skip the sending interface */
			if (grpintn->interface == intn) continue;

			/* Add a group record if we didn't do so already */
			if (grec_added == 0)
			{
			}

			/* Copy the address into the packet */
			/* Hmmm... not implemented completely yet.... */
		}
		
		/* Fixup the number of addresses */
	}

	/* Calculate and fill in the checksum */
	packet.mld1.csum		= ipv6_checksum(&packet.ip6, IPPROTO_ICMPV6, (uint8_t *)&packet.mld1, sizeof(packet.mld1));

	dolog(LOG_DEBUG, "Sending MLDv2 Report on %s\n", intn->name);
	sendpacket6(intn, (const struct ip6_hdr *)&packet, sizeof(packet));

	/* Increase ICMP sent statistics */
	g_conf->stat_icmp_sent++;
	intn->stat_icmp_sent++;
}
#endif /* ECMH_SUPPORT_MLD2 */

void mld_send_report(struct intnode *intn, const struct in6_addr *mca)
{
	/*
	 * When we haven't detected a querier on a link
	 * send reports as both MLDv1 and MLDv2, as
	 * listeners (hosts) might be interrested.
	 */
	if (intn->mld_version == 0 || intn->mld_version == 1)
	{
		mld1_send_report(intn, mca);
	}
#ifdef ECMH_SUPPORT_MLD2
	if (intn->mld_version == 0 || intn->mld_version == 2)
	{
		mld2_send_report(intn, mca);
	}
#endif /* EMCH_SUPPORT_MLD2 */
}

#ifdef ECMH_SUPPORT_IPV4

/* IPv4 ICMP */
void l4_ipv4_icmp(struct intnode *intn, struct ip *iph, const uint8_t *packet, const uint16_t len)
{
	D(dolog(LOG_DEBUG, "%5s L4:IPv4 ICMP\n", intn->name);)
	return;
}

#endif /* ECMH_SUPPORT_IPV4 */

#ifdef ECMH_BPF
/*
 * Protocol 41 - IPv6 in IPv4 (RFC3065)
 *
 * We decapsulate these packets and then feed them to ecmh again
 * This way we don't have to a BPF/PCAP on all the tunnel interfaces
 * 
 */
void l4_ipv4_proto41(struct intnode *intn, struct ip *iph, const uint8_t *packet, const uint16_t len)
{
	struct localnode 	*localn;
	struct intnode		*tun;

	/* Ignore when we are not in tunnelmode */
	if (!g_conf->tunnelmode) return;

	/* Is this a locally sourced packet? */
	localn = local_find(&iph->ip_src);
	/* Ignore when local */
	if (localn)
	{
#if 0
		dolog(LOG_DEBUG, "Dropping packet originating from ourselves on %s\n", intn->name);
#endif
		return;
	}

	/* Find the matching interface which is actually sending this */
	tun = int_find_ipv4(false, &iph->ip_src, true);

	if (!tun)
	{
		/* Try to update the list */
		update_interfaces(NULL);

		/* Try to find it again */
		tun = int_find_ipv4(false, &iph->ip_src, true);
	}
	if (!tun)
	{
	    	char buf[1024],buf2[1024];
	    	inet_ntop(AF_INET, &iph->ip_src, (char *)&buf, sizeof(buf));
	    	inet_ntop(AF_INET, &iph->ip_dst, (char *)&buf2, sizeof(buf));
		dolog(LOG_ERR, "Couldn't find proto-41 tunnel %s->%s\n", buf);
		return;
	}

	/* Send it through our decoder again, looking as it is a native IPv6 received on intn ;) */
	dolog(LOG_DEBUG, "Proto-41 from %x->%x on %s, tunnel %s\n", iph->ip_src, iph->ip_dst, intn->name, tun->name);
	l2_ethtype(tun, packet, len, ETH_P_IPV6);
	return;
}
#endif /* ECMH_BPF */

/* IPv4 */
void l3_ipv4(struct intnode *intn, struct ip *iph, const uint16_t len)
{
	if (iph->ip_v != 4)
	{
		D(dolog(LOG_DEBUG, "%5s L3:IPv4: IP version %u not supported\n", intn->name, iph->ip_v);)
		return;
	}

	if (iph->ip_hl < 5)
	{
		D(dolog(LOG_DEBUG, "%5s L3IPv4: IP hlen < 5 bytes (%u)\n", intn->name, iph->ip_hl);)
		return;
	}

	if (ntohs(iph->ip_len) > len)
	{
		/* This happens mostly with unknown ARPHRD_* types */
		D(dolog(LOG_DEBUG, "%5s L3:IPv4: *** L3 length > L2 length (%u != %u)\n", intn->name, ntohs(iph->ip_len), len);)
#if 0
		return;
#endif
	}
/*
D(
	inet_ntop(AF_INET, &iph->ip_src, src, sizeof(src));
	inet_ntop(AF_INET, &iph->ip_dst, dst, sizeof(dst));

	dolog(LOG_DEBUG, "%5s L3:IPv4: IPv%01u %-16s %-16s %4u\n", intn->name, iph->ip_v, src, dst, ntohs(iph->ip_len));
)
*/

	/* Go to Layer 4 */
#ifdef ECMH_SUPPORT_IPV4
	if (iph->ip_p == 1) l4_ipv4_icmp(intn, iph, ((uint8_t *iph)+(4*iph->ip_hl), len-(4*iph->ip_hl));
#ifdef ECMH_BPF
	else
#endif /* ECMH_BPF */
#endif /* ECMH_SUPPORT_IPV4 */
#ifdef ECMH_BPF
	if (iph->ip_p == IPPROTO_IPV6) l4_ipv4_proto41(intn, iph, ((uint8_t *)iph)+(4*iph->ip_hl), len-(4*iph->ip_hl));
#endif /* ECMH_BPF */
}

void mld_log(unsigned int level, char *fmt, struct in6_addr *i_mca, const struct intnode *intn)
{
	char mca[INET6_ADDRSTRLEN];
	memset(mca,0,sizeof(mca));
	inet_ntop(AF_INET6, i_mca, mca, sizeof(mca));
	dolog(level, fmt, mca, intn->name);
}

void l4_ipv6_icmpv6_mld1_report(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld1 *mld1, const uint16_t plen)
{
	struct grpintnode	*grpintn;
	struct in6_addr		any;

	/*
	 * We have received a MLDv1 report, thus note this
	 * interface as having a MLDv1 listener
	 */
	int_set_mld_version(intn, 1);

	mld_log(LOG_DEBUG, "Received a ICMPv6 MLDv1 Report for %s on %s\n", &mld1->mca, intn);

	/*
	 * Ignore groups:
	 * - non multicast 
	 * - node local multicast addresses
	 * - link local multicast addresses
	 * - multicast destination mismatch with ipv6 destination
	 */
	if (	!IN6_IS_ADDR_MULTICAST(&mld1->mca) ||
		IN6_IS_ADDR_MC_NODELOCAL(&mld1->mca) ||
		IN6_IS_ADDR_MC_LINKLOCAL(&mld1->mca)) return;

	/* Find the grpintnode or create it */
	grpintn = groupint_get(&mld1->mca, intn);
	if (!grpintn)
	{
		mld_log(LOG_WARNING, "Couldn't find or create new group %s for %s\n", &mld1->mca, intn);
		return;
	}

	/* No source address, so use any */
	memset(&any, 0, sizeof(any));
	
	if (!grpint_refresh(grpintn, &any, MLD2_MODE_IS_INCLUDE))
	{
		mld_log(LOG_WARNING, "Couldn't create subscription to %s for %s\n", &mld1->mca, intn);
		return;
	}

	return;
}

void l4_ipv6_icmpv6_mld1_reduction(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld1 *mld1, const uint16_t plen)
{
	struct groupnode	*groupn;
	struct grpintnode	*grpintn;
	struct in6_addr		any;

	/*
	 * We have received a MLDv1 reduction, thus note this
	 * interface as having a MLDv1 listener
	 */
	int_set_mld_version(intn, 1);

	mld_log(LOG_DEBUG, "Received a ICMPv6 MLDv1 Reduction for %s on %s\n", &mld1->mca, intn);

	/*
	 * Ignore groups:
	 * - non multicast 
	 * - node local multicast addresses
	 * - link local multicast addresses
	 */
	if (	!IN6_IS_ADDR_MULTICAST(&mld1->mca) ||
		IN6_IS_ADDR_MC_NODELOCAL(&mld1->mca) ||
		IN6_IS_ADDR_MC_LINKLOCAL(&mld1->mca)) return;

	/* Find the groupnode */
	groupn = group_find(&mld1->mca);
	if (!groupn)
	{
		mld_log(LOG_WARNING, "Couldn't find group %s for reduction of %s\n", &mld1->mca, intn);
		return;
	}

	/* Find the grpintnode */
	grpintn = grpint_find(groupn->interfaces, intn);
	if (!grpintn)
	{
		mld_log(LOG_WARNING, "Couldn't find the grpint %s for reduction of %s\n", &mld1->mca, intn);
		return;
	}

	/* No source address, so use any */
	memset(&any, 0, sizeof(any));

	if (!subscr_unsub(grpintn->subscriptions, &any))
	{
		mld_log(LOG_WARNING, "Couldn't unsubscribe from %s interface %s\n", &mld1->mca, intn);
		return;
	}

	if (grpintn->subscriptions->count <= 0)
	{
		/* Requery if somebody still want it, as it will timeout otherwise. */
		mld_log(LOG_DEBUG, "Querying for other listeners to %s on interface %s\n", &mld1->mca, intn);
#ifndef ECMH_SUPPORT_MLD2
		mld_send_query(intn, &mld1->mca, NULL);
#else
		mld_send_query(intn, &mld1->mca, NULL, false);
		/* Skip Robustness */
		grpintn->subscriptions->count = -ECMH_ROBUSTNESS_FACTOR;
#endif
	}

	return;
}

#ifdef ECMH_SUPPORT_MLD2
void l4_ipv6_icmpv6_mld2_report(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld2_report *mld2r, const uint16_t plen)
{
	char			mca[INET6_ADDRSTRLEN], srct[INET6_ADDRSTRLEN];
	struct grpintnode	*grpintn = NULL;
	struct in6_addr		*src, any;
	struct mld2_grec	*grec = (struct mld2_grec *)(((char *)mld2r)+sizeof(*mld2r));
	unsigned int ngrec	= ntohs(mld2r->ngrec), nsrcs = 0;

	/*
	 * We have received a MLDv2 report, thus note this
	 * interface as having a MLDv2 listener, unless it
	 * has detected MLDv1 listeners already...
	 */
	int_set_mld_version(intn, 2);

	dolog(LOG_DEBUG, "Received a ICMPv6 MLDv2 Report (%u) on %s (grec's: %u)\n",
		(unsigned int)mld2r->type, intn->name, ngrec);

	if ((sizeof(*mld2r) + ngrec*sizeof(*grec)) > plen)
	{
		dolog(LOG_ERR, "Ignoring packet with invalid number of Group Records (would exceed packetlength)\n");
		return;
	}

	/* Zero out just in case */
	memset(&mca, 0, sizeof(mca));
	memset(&srct, 0, sizeof(srct));
	memset(&any, 0, sizeof(any));

	while (ngrec > 0)
	{
		/* Check if we are still inside the packet */
		if (((char *)grec) > (((char *)mld2r)+plen))
		{
			dolog(LOG_ERR, "Reached outside the packet (ngrec=%u) received on %s, length %u -> ignoring\n",
				ngrec, intn->name, plen);
			return;
		}

		grpintn = NULL;

		nsrcs = ntohs(grec->grec_nsrcs);
		src = (struct in6_addr *)(((char *)grec) + sizeof(*grec));

		if (	grec->grec_type != MLD2_MODE_IS_INCLUDE &&
			grec->grec_type != MLD2_MODE_IS_EXCLUDE &&
			grec->grec_type != MLD2_CHANGE_TO_INCLUDE &&
			grec->grec_type != MLD2_CHANGE_TO_EXCLUDE &&
			grec->grec_type != MLD2_ALLOW_NEW_SOURCES &&
			grec->grec_type != MLD2_BLOCK_OLD_SOURCES)
		{
			dolog(LOG_ERR, "Unknown Group Record Type %u/0x%x (ngrec=%u) on %s -> Ignoring Report\n",
				grec->grec_type, grec->grec_type, ngrec, intn->name);
			return;
		}

#ifdef DEBUG
		inet_ntop(AF_INET6, &grec->grec_mca, mca, sizeof(mca));
		dolog(LOG_DEBUG, "MLDv2 Report (grec=%u) wanting %s %s with %u sources on %s\n",
			ngrec, lookup(mld2_grec_types, grec->grec_type),
			mca, nsrcs, intn->name);
#endif

		/* Ignore node and link local multicast addresses */
		if (	!IN6_IS_ADDR_MC_NODELOCAL(&grec->grec_mca) &&
			!IN6_IS_ADDR_MC_LINKLOCAL(&grec->grec_mca))
		{
			inet_ntop(AF_INET6, &grec->grec_mca, mca, sizeof(mca));

			/* Find the grpintnode or create it */
			grpintn = groupint_get(&grec->grec_mca, intn);
			if (!grpintn)
			{
				mld_log(LOG_WARNING, "L4:IPv6:ICMPv6:MLD2_Report Couldn't find or create new group for %s on %s\n", &grec->grec_mca, intn);
			}
		}

		if (nsrcs == 0)
		{
			if (grpintn)
			{
				if (!grpint_refresh(grpintn, &any,
					grec->grec_type == MLD2_MODE_IS_EXCLUDE ||
					grec->grec_type == MLD2_CHANGE_TO_EXCLUDE ||
					grec->grec_type == MLD2_BLOCK_OLD_SOURCES ?
					MLD2_MODE_IS_INCLUDE : MLD2_MODE_IS_EXCLUDE))
				{
					mld_log(LOG_WARNING, "Couldn't create subscription to %s for %s\n",
						&grec->grec_mca, intn);
					return;
				}
			}
		}
		else
		{
			if ((((char *)src)-((char *)mld2r) + (nsrcs*sizeof(*src))) > plen)
			{
				dolog(LOG_ERR, "Ignoring packet with invalid number (%u) of sources (would exceed packetlength)\n", nsrcs);
				return;
			}

			/* Do all source addresses */
			while (nsrcs > 0)
			{
				/* Skip if we didn't get a grpint */
				if (grpintn)
				{
					if (!grpint_refresh(grpintn, src, grec->grec_type))
					{
						mld_log(LOG_ERR, "Couldn't subscribe sourced from %s on %s\n", src, intn);
					}
				}

				/* Next src */
				src = (struct in6_addr *)(((char *)src) + sizeof(*src));
				nsrcs--;
			}

			/* Step over the source, thus pointing at the next grec */
			src = (struct in6_addr *)(((char *)src) + sizeof(*src));
		}

		/* Next grec, also skip the auxwords */
		grec = (struct mld2_grec *)(((char *)src) + grec->grec_auxwords);
		ngrec--;
	}

	return;
}
#endif /* ECMH_SUPPORT_MLD2 */

void l4_ipv6_icmpv6_mld_query(struct intnode *intn, const struct ip6_hdr *iph, const uint16_t len, struct mld2_query *mld, const uint16_t plen)
{
	struct groupnode	*groupn;
	struct listnode		*ln;
	struct grpintnode	*grpintn;
	struct listnode		*gn;

	dolog(LOG_DEBUG, "Received a ICMPv6 MLD Query on %s\n", intn->name);
	
	/* It's MLDv1 when the packet has the size of a MLDv1 packet */
	if (plen == sizeof(struct mld1)) int_set_mld_version(intn, 1);

	/*
	 * It is MLDv2 (or up) when it has anything else
	 * It could be MLDv3 if that ever comes out, but we don't know.
	 */
	else int_set_mld_version(intn, 2);

#ifdef ECMH_SUPPORT_MLD2
	/*
	 * If it is a yet unknown MLD send MLDv1's
	 * Unknown MLD's should not happen though
	 * as the above code just determined what
	 * version it is.
	 */
	if (intn->mld_version == 0 || intn->mld_version == 1)
	{
#endif /* ECMH_SUPPORT_MLD2 */
		/* MLDv1 sends reports one group at a time */

		/*
		 * Walk along the list of groups
		 * and report all the groups we are subscribed for
		 */
		LIST_LOOP(g_conf->groups, groupn, ln)
		{
			LIST_LOOP(groupn->interfaces, grpintn, gn)
			{
				/* We only are sending for this interface */
				if (grpintn->interface != intn) continue;
	
				/* Report this group to the querying router */
				mld_send_report(intn, &groupn->mca);
			}
		}
#ifdef ECMH_SUPPORT_MLD2
	}
	else
	{
		/* mld2_send_report_all(intn); */
	}
#endif /* ECMH_SUPPORT_MLD2 */

	return;
}

/*
 * Forward a multicast packet to interfaces that have subscriptions for it
 *
 * intn		= The interface we received this packet on
 * packet	= The packet, starting with IPv6 header
 * len		= Length of the complete packet
 */
void l4_ipv6_multicast(struct intnode *intn, struct ip6_hdr *iph, const uint16_t len)
{
	struct groupnode	*groupn;
	struct grpintnode	*grpintn;
	struct subscrnode	*subscrn;
	struct listnode		*in, *in2;

	/* 
	 * Don't route multicast packets that:
	 * - src = multicast
	 * - src = unspecified
	 * - dst = unspecified
	 * - src = linklocal
	 * - dst = node local multicast
	 * - dst = link local multicast
	 */
	if (	IN6_IS_ADDR_MULTICAST(&iph->ip6_src) ||
		IN6_IS_ADDR_UNSPECIFIED(&iph->ip6_src) ||
		IN6_IS_ADDR_UNSPECIFIED(&iph->ip6_dst) ||
		IN6_IS_ADDR_LINKLOCAL(&iph->ip6_src) ||
		IN6_IS_ADDR_MC_NODELOCAL(&iph->ip6_dst) ||
		IN6_IS_ADDR_MC_LINKLOCAL(&iph->ip6_dst)) return;
#if 0
D(
	{
		/* DEBUG - Causes a lot of debug output - thus disabled even for debugging */
		char src[INET6_ADDRSTRLEN];
		char dst[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &iph->ip6_src, src, sizeof(src));
		inet_ntop(AF_INET6, &iph->ip6_dst, dst, sizeof(dst));
		dolog(LOG_DEBUG, "%5s L3:IPv6: IPv%0x %40s %40s %4u %u\n", intn->name, (int)((iph->ip6_vfc>>4)&0x0f), src, dst, ntohs(iph->ip6_plen), iph->ip6_nxt);
	}
)
#endif

	/* Find the group belonging to this multicast destination */
	groupn = group_find(&iph->ip6_dst);

	if (!groupn)
	{
		/* Causes a lot of debug output, be warned */
#if 0
		char src[INET6_ADDRSTRLEN];
		char dst[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &iph->ip6_src, src, sizeof(src));
		inet_ntop(AF_INET6, &iph->ip6_dst, dst, sizeof(dst));
		mld_log(LOG_DEBUG, "No subscriptions for %s (sent by %s)\n",  dst, src);
#endif
		return;
	}

	LIST_LOOP(groupn->interfaces, grpintn, in)
	{
		/* Don't send to the interface this packet originated from */
		if (intn->ifindex == grpintn->interface->ifindex) continue;

		/* Check the subscriptions for this group */
		LIST_LOOP(grpintn->subscriptions, subscrn, in2)
		{
			/* Unspecified or specific subscription to this address? */
			if (	IN6_ARE_ADDR_EQUAL(&subscrn->ipv6, &in6addr_any) ||
				IN6_ARE_ADDR_EQUAL(&subscrn->ipv6, &iph->ip6_src))
			{
				/*
				 * If it was explicitly requested to include
				 * packets from this source,
				 * don't even look further and do so.
				 * This is the case when an MLDv1 listener
				 * is on the link too fe.
				 */
				if (subscrn->mode == MLD2_MODE_IS_INCLUDE)
				{
					sendpacket6(grpintn->interface, iph, len);
					break;
				}
			}
		}

	}
}

/*
 * Check the ICMPv6 message for MLD's or ICMP echo requests
 *
 * intn		= The interface we received this packet on
 * packet	= The packet, starting with IPv6 header
 * len		= Length of the complete packet
 * data		= the payload
 * plen		= Payload length (should match up to at least icmpv6
 */
void l4_ipv6_icmpv6(struct intnode *intn, struct ip6_hdr *iph, const uint16_t len, struct icmp6_hdr *icmpv6, const uint16_t plen)
{
	uint16_t		csum;

	/* Increase ICMP received statistics */
	g_conf->stat_icmp_received++;
	intn->stat_icmp_received++;

	/*
	 * We are only interrested in these types
	 * Saves on calculating a checksum and then ignoring it anyways
	 */
	if (	icmpv6->icmp6_type != ICMP6_MEMBERSHIP_REPORT &&
		icmpv6->icmp6_type != ICMP6_MEMBERSHIP_REDUCTION &&
#ifdef ECMH_SUPPORT_MLD2
		icmpv6->icmp6_type != ICMP6_V2_MEMBERSHIP_REPORT &&
		icmpv6->icmp6_type != ICMP6_V2_MEMBERSHIP_REPORT_EXP &&
#endif
		icmpv6->icmp6_type != ICMP6_MEMBERSHIP_QUERY &&
		icmpv6->icmp6_type != ICMP6_ECHO_REQUEST)
	{
		dolog(LOG_DEBUG, "Ignoring ICMPv6: %s (%u), %s (%u) received on %s\n",
			icmpv6_type(icmpv6->icmp6_type), icmpv6->icmp6_type,
			icmpv6_code(icmpv6->icmp6_type, icmpv6->icmp6_code), icmpv6->icmp6_code,
			intn->name);
		return;
	}

	/* Save the checksum */
	csum = icmpv6->icmp6_cksum;
	/* Clear it temporarily */
	icmpv6->icmp6_cksum = 0;

	/* Verify checksum */
	icmpv6->icmp6_cksum = ipv6_checksum(iph, IPPROTO_ICMPV6, (uint8_t *)icmpv6, plen);
	if (icmpv6->icmp6_cksum != csum)
	{
		dolog(LOG_WARNING, "CORRUPT->DROP (%s): Received a ICMPv6 %s (%u) with wrong checksum (%x vs %x)\n",
			intn->name,
			icmpv6_type(icmpv6->icmp6_type), icmpv6->icmp6_type,
			icmpv6_code(icmpv6->icmp6_type, icmpv6->icmp6_code), icmpv6->icmp6_code,
			icmpv6->icmp6_cksum, csum);
	}

	if (icmpv6->icmp6_type == ICMP6_ECHO_REQUEST)
	{
		/*
		 * We redistribute IPv6 ICMPv6 Echo Requests to the subscribers
		 * This allows hosts to ping a IPv6 Multicast address and see who is listening ;) 
		 */

		/* Decrease the hoplimit, but only if not 0 yet */
		if (iph->ip6_hlim > 0) iph->ip6_hlim--;
		/* else dolog(LOG_DEBUG, "Hoplimit for ICMPv6 packet was already %u\n", iph->ip6_hlim);*/
		if (iph->ip6_hlim == 0)
		{
			g_conf->stat_hlim_exceeded++;
			/* Send a time_exceed_transit error */
			icmp6_send(intn, &iph->ip6_src, ICMP6_ECHO_REPLY, ICMP6_TIME_EXCEED_TRANSIT, &icmpv6->icmp6_data32, plen-sizeof(*icmpv6)+sizeof(icmpv6->icmp6_data32));
			return;
		}
		/* Send this packet along it's way */
		else l4_ipv6_multicast(intn, iph, len);
	}
	else
	{
		if (!(IN6_IS_ADDR_LINKLOCAL(&iph->ip6_src)))
		{
			mld_log(LOG_WARNING, "Ignoring non-LinkLocal MLD from %s received on %s\n", &iph->ip6_src, intn);
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
#ifdef ECMH_SUPPORT_MLD2
		else if (icmpv6->icmp6_type == ICMP6_V2_MEMBERSHIP_REPORT ||
			 icmpv6->icmp6_type == ICMP6_V2_MEMBERSHIP_REPORT_EXP)
		{
			l4_ipv6_icmpv6_mld2_report(intn, iph, len, (struct mld2_report *)icmpv6, plen);
		}
#endif /* ECMH_SUPPORT_MLD2 */
		else if (icmpv6->icmp6_type == ICMP6_MEMBERSHIP_QUERY)
		{
			l4_ipv6_icmpv6_mld_query(intn, iph, len, (struct mld2_query *)icmpv6, plen);
		}
		else
		{
			dolog(LOG_DEBUG, "ICMP type %s (%u), %s (%u) got through\n",
				icmpv6_type(icmpv6->icmp6_type), icmpv6->icmp6_type,
				icmpv6_code(icmpv6->icmp6_type, icmpv6->icmp6_code), icmpv6->icmp6_code);
		}
	}
	return;
}

void l3_ipv6(struct intnode *intn, struct ip6_hdr *iph, const uint16_t len)
{
	struct ip6_ext		*ipe;
	uint8_t			ipe_type;
	uint16_t		plen;
	uint32_t		l;

	/*
	 * Destination must be multicast
	 * We don't care about unicast destinations
	 * Those are handled by the OS itself hopefully ;)
	 */
	if (!IN6_IS_ADDR_MULTICAST(&iph->ip6_dst))
	{
		/* printf("Address is not multicast!\n"); */
		return;
	}

	/* Source should not be us (linklocal/global) */
	if (	memcmp(&iph->ip6_src, &intn->linklocal, sizeof(iph->ip6_dst)) == 0 ||
		memcmp(&iph->ip6_src, &intn->global, sizeof(iph->ip6_dst)) == 0)
	{
		dolog(LOG_DEBUG, "Skipping packet from own host on %s\n", intn->name);
		return;
	}

	/* Save the type of the next header */
	ipe_type = iph->ip6_nxt;
	/* Step to the next header */
	ipe = (struct ip6_ext *)(((char *)iph) + sizeof(*iph));
	plen = ntohs(iph->ip6_plen);

	/* Skip the headers that we know */
	while (	ipe_type == IPPROTO_HOPOPTS ||
		ipe_type == IPPROTO_ROUTING ||
		ipe_type == IPPROTO_DSTOPTS ||
		ipe_type == IPPROTO_AH)
	{
		/* Save the type of the next header */
		ipe_type = ipe->ip6e_nxt;

		/* Step to the next header */
		l = ((ipe->ip6e_len*8)+8);
		plen -= l;
		ipe  = (struct ip6_ext *)(((char *)ipe) + l);

		/* Check for corrupt packets */
		if ((char *)ipe > (((char *)iph)+len))
		{
			dolog(LOG_WARNING, "CORRUPT->DROP (%s): Header chain beyond packet data\n", intn->name);
			return;
		}
	}

	/* Check for ICMP */
	if (ipe_type == IPPROTO_ICMPV6)
	{
		/* Take care of ICMPv6 */
		l4_ipv6_icmpv6(intn, iph, len, (struct icmp6_hdr *)ipe, plen);
		return;
	}

	/* Handle multicast packets */
	if (IN6_IS_ADDR_MULTICAST(&iph->ip6_dst))
	{
		/* Decrease the hoplimit, but only if not 0 yet */
		if (iph->ip6_hlim > 0) iph->ip6_hlim--;
		D(else dolog(LOG_DEBUG, "Hoplimit for UDP packet was already %u\n", iph->ip6_hlim);)
		if (iph->ip6_hlim == 0)
		{
			g_conf->stat_hlim_exceeded++;
			return;
		}

		l4_ipv6_multicast(intn, iph, len);
		return;
	}

	/* Ignore the rest */
	return;
}

void l2_ethtype(struct intnode *intn, const uint8_t *packet, const unsigned int len, const unsigned int ether_type)
{
	if (ether_type == ETH_P_IP)
	{
		l3_ipv4(intn, (struct ip	*)packet, len);
	}
	else if (ether_type == ETH_P_IPV6)
	{
		l3_ipv6(intn, (struct ip6_hdr	*)packet, len);
	}
	/* We don't care about anything else... */
}

void l2_eth(struct intnode *intn, struct ether_header *eth, const unsigned int len)
{
	l2_ethtype(intn, ((uint8_t *)eth + sizeof(*eth)), len-sizeof(*eth), ntohs(eth->ether_type));
}

/* Initiliaze interfaces */
void update_interfaces(struct intnode *intn)
{
	struct intnode		*specific = intn;
	char			buf[100];
	struct in6_addr		addr;
	unsigned int		ifindex = 0;
	int			newintn	= false;
#ifndef ECMH_GETIFADDR
	FILE			*file;
	unsigned int		prefixlen, scope, flags;
	char			devname[IFNAMSIZ];
#else
	struct ifaddrs		*ifap, *ifa;
	unsigned int		i = 0;

#endif /* !ECMH_GETIFADDR */
	int			gotlinkl = false, gotglobal = false;

	dolog(LOG_DEBUG, "Updating Interfaces\n");

#ifndef ECMH_GETIFADDR
	/* Get link local addresses from /proc/net/if_inet6 */
	file = fopen("/proc/net/if_inet6", "r");

	/* We can live without it though */
	if (!file)
	{
		dolog(LOG_WARNING, "Couldn't open /proc/net/if_inet6 for figuring out local interfaces\n");
		return;
	}

	/* Format "fe80000000000000029027fffe24bbab 02 0a 20 80     eth0" */
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
#else /* !ECMH_GETIFADDR */
	/* FreeBSD etc style */
	if (getifaddrs(&ifap) == 0)
	{
		for (ifa=ifap;ifa;ifa=ifa->ifa_next)
		{
			/*
			 * Ignore:
			 * - loopbacks
			 * - devices that are not up
			 * - devices that are not running
			 */
			if (	((ifa->ifa_flags & IFF_LOOPBACK)	== IFF_LOOPBACK) ||
				((ifa->ifa_flags & IFF_UP) 		!= IFF_UP) ||
				((ifa->ifa_flags & IFF_RUNNING) 	!= IFF_RUNNING)
#ifdef ECMH_BPF
				|| ifa->ifa_addr->sa_family == AF_LINK
#endif
				)
			{
#if 0
				dolog(LOG_DEBUG, "%s %u%s%s%s%s (%u) -> ignoring\n",
					ifa->ifa_name,
					ifa->ifa_addr->sa_family,
					(ifa->ifa_flags & IFF_UP) == IFF_UP ? " Up": "",
					(ifa->ifa_flags & IFF_RUNNING) == IFF_RUNNING ? " Running" : "",
					(ifa->ifa_flags & IFF_LOOPBACK) == IFF_LOOPBACK ? " Loopback" : "",
					(ifa->ifa_flags & IFF_POINTOPOINT) == IFF_POINTOPOINT ? " PtP" : "",
					ifa->ifa_flags
					);
#endif
				continue;
			}
#if 0
			dolog(LOG_DEBUG, "%s %u%s%s%s%s (%u) -> trying...\n",
				ifa->ifa_name,
				ifa->ifa_addr->sa_family,
				(ifa->ifa_flags & IFF_UP) == IFF_UP ? " Up": "",
				(ifa->ifa_flags & IFF_RUNNING) == IFF_RUNNING ? " Running" : "",
				(ifa->ifa_flags & IFF_LOOPBACK) == IFF_LOOPBACK ? " Loopback" : "",
				(ifa->ifa_flags & IFF_POINTOPOINT) == IFF_POINTOPOINT ? " PtP" : "",
				ifa->ifa_flags
				);
#endif

			ifindex = if_nametoindex(ifa->ifa_name);
			if (ifa->ifa_addr)
			{
				if (ifa->ifa_addr->sa_family == AF_INET6)
				{
					memcpy(&addr,
						&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr, sizeof(addr));
				}
				else
				{
					memcpy(&addr,
						&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, sizeof(addr));
				}
			}
			else
			{
				dolog(LOG_WARNING, "Interface %s/%u didn't have an address!? -> skipping\n", ifa->ifa_name, ifindex);
				continue;
			}
#endif /* !ECMH_GETIFADDR */
		/* Skip everything we don't care about */
		if (
#ifdef ECMH_GETIFADDR
				ifa->ifa_addr->sa_family == AF_INET6 && (
#endif
				!IN6_IS_ADDR_LINKLOCAL(&addr) &&
				(
					IN6_IS_ADDR_UNSPECIFIED(&addr) ||
					IN6_IS_ADDR_LOOPBACK(&addr) ||
					IN6_IS_ADDR_MULTICAST(&addr))
				)
#ifdef ECMH_GETIFADDR
			)
#endif /* !ECMH_GETIFADDR */
		{
#if 0
				char txt[INET6_ADDRSTRLEN];
				memset(txt,0,sizeof(txt));
				inet_ntop(AF_INET6, &addr, txt, sizeof(txt));
				dolog(LOG_DEBUG, "Ignoring other IPv6 address %s on interface %s\n", txt,
#ifndef ECMH_GETIFADDR
					devname
#else
					ifa->ifa_name
#endif /* !ECMH_GETIFADDR */
				);
#endif /* if 0 */
			continue;
		}

		if (specific)
		{
			intn = specific;
			if (intn->ifindex == ifindex) continue;
		}
		else
		{
			newintn = gotlinkl = gotglobal = false;
			intn = int_find(ifindex, false);

			/* Not Found? -> Create the interface */
			if (	!intn &&
#ifndef ECMH_BPF
				(intn = int_create(ifindex)))
#else
				(intn = int_create(ifindex,
					(IFF_POINTOPOINT == (ifa->ifa_flags & IFF_POINTOPOINT)) ? true : false)))
#endif
			{
				newintn = true;
			}
		}

		/* Did the find or creation succeed ? */
		if (intn)
		{
			/* Link Local IPv6 address ? */
			if (
#ifdef ECMH_GETIFADDR
				ifa->ifa_addr->sa_family == AF_INET6 &&
#endif
				IN6_IS_ADDR_LINKLOCAL(&addr))
			{
				/* Update the linklocal address */
				memcpy(&intn->linklocal, &addr, sizeof(intn->linklocal));
				gotlinkl = true;
			}
			else
			{
#ifdef ECMH_BPF
				if (ifa->ifa_addr->sa_family == AF_INET)
				{
					/* Update the Local IPv4 address */
					dolog(LOG_INFO, "Updating local IPv4 address for %s\n", intn->name);
					memcpy(&intn->ipv4_local, &addr, sizeof(intn->ipv4_local));

					if ((ifa->ifa_flags & IFF_POINTOPOINT) != IFF_POINTOPOINT)
					{
						/* Update the locals list */
						local_update(intn);
					}
				}
				else if (ifa->ifa_addr->sa_family == AF_INET6)
				{
#endif
					/* Update the global address */
					dolog(LOG_INFO, "Updating global IPv6 address for %s\n", intn->name);
					memcpy(&intn->global, &addr, sizeof(intn->global));
					gotglobal = true;
#ifdef ECMH_BPF
				}
				else
				{
					dolog(LOG_ERR, "Unknown Address Family %u - Ignoring\n", ifa->ifa_addr->sa_family);
				}
#endif
			}
		}

		if (specific)
		{
			/* Are we done updating? */
			if (gotlinkl && gotglobal) break;
		}
		else
		{
			/* Add it to the list if it is a new one and */
			/* either the linklocal or global addresses are set. */
			if (newintn)
			{
				if (gotlinkl || gotglobal) int_add(intn);
				else int_destroy(intn);
			}
		}
#ifdef ECMH_GETIFADDR
		}
		freeifaddrs(ifap);
#endif
	}
	dolog(LOG_DEBUG, "Updating Interfaces - done\n");
#ifndef ECMH_GETIFADDR
	fclose(file);
#else
#endif
}

void init()
{
	g_conf = malloc(sizeof(struct conf));
	if (!g_conf)
	{
		dolog(LOG_ERR, "Couldn't init()\n");
		exit(-1);
	}

	/* Clear it, never bad, always good  */
	memset(g_conf, 0, sizeof(*g_conf));

	/*
	 * 32k of buffer should be enough
	 * we can then have ~30 packets in
	 * the same buffer-read
	 */
	g_conf->bufferlen		= (32*1024);

#ifndef ECMH_BPF
	/* Raw socket is not open yet */
	g_conf->rawsocket		= -1;
#else
	FD_ZERO(&g_conf->selectset);
	g_conf->tunnelmode		= true;
	g_conf->locals			= list_new();
	g_conf->locals->del 		= (void(*)(void *))local_destroy;
#endif /* ECMH_BPF */

	/* Initialize our configuration */
	g_conf->maxgroups		= 42;		/* FIXME: Not verified yet... */
	g_conf->daemonize		= true;

	/* Initialize our list of interfaces */
	g_conf->ints			= list_new();
	g_conf->ints->del 		= (void(*)(void *))int_destroy;

	/* Initialize our list of groups */
	g_conf->groups			= list_new();
	g_conf->groups->del		= (void(*)(void *))group_destroy;

	/* Initialize our counters */
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
	/* Reset the signal */
	signal(i, &sighup);
}

/* Dump the statistical information */
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
	unsigned int		subscriptions = 0;
	unsigned int		uptime_s, uptime_m, uptime_h, uptime_d;

	/* Get the current time */
	time_tee  = time(NULL);
	uptime_s  = time_tee - g_conf->stat_starttime;
	uptime_d  = uptime_s / (24*60*60);
	uptime_s -= uptime_d *  24*60*60;
	uptime_h  = uptime_s / (60*60);
	uptime_s -= uptime_h *  60*60;
	uptime_m  = uptime_s /  60;
	uptime_s -= uptime_m *  60;

	/* Rewind the file to the start */
	rewind(g_conf->stat_file);

	/* Truncate the file */
	ftruncate(fileno(g_conf->stat_file), (off_t)0);

	/* Dump out all the groups with their information */
	fprintf(g_conf->stat_file, "*** Subscription Information Dump\n");

	LIST_LOOP(g_conf->groups, groupn, ln)
	{
		inet_ntop(AF_INET6, &groupn->mca, addr, sizeof(addr));
		fprintf(g_conf->stat_file, "Group: %s\n", addr);

		LIST_LOOP(groupn->interfaces, grpintn, gn)
		{
			fprintf(g_conf->stat_file, "\tInterface: %s (%d)\n",
				grpintn->interface->name, grpintn->subscriptions->count);

			LIST_LOOP(grpintn->subscriptions, subscrn, ssn)
			{
				int i = time_tee - subscrn->refreshtime;
				if (i < 0) i = -i;

				inet_ntop(AF_INET6, &subscrn->ipv6, addr, sizeof(addr));
				fprintf(g_conf->stat_file, "\t\t%s %s (%u seconds old)\n",
					addr,
					subscrn->mode == MLD2_MODE_IS_INCLUDE ? "INCLUDE" : "EXCLUDE",
					i);

				subscriptions++;
			}
		}
	}

	fprintf(g_conf->stat_file, "*** Subscription Information Dump (end - %u groups, %u subscriptions)\n", g_conf->groups->count, subscriptions);
	fprintf(g_conf->stat_file, "\n");

	/* Dump all the interfaces */
	fprintf(g_conf->stat_file, "*** Interface Dump\n");

	LIST_LOOP(g_conf->ints, intn, ln)
	{
		fprintf(g_conf->stat_file, "\n");
		fprintf(g_conf->stat_file, "Interface: %s\n", intn->name);
		fprintf(g_conf->stat_file, "  Index number           : %u\n", intn->ifindex);
		fprintf(g_conf->stat_file, "  MTU                    : %u\n", intn->mtu);

#ifdef ECMH_BPF
		/* Tunnel has a master interface? */
		if (intn->master)
		{
			inet_ntop(AF_INET, &intn->master->ipv4_local, addr, sizeof(addr));
			fprintf(g_conf->stat_file, "  Master interface       : %s (%u/%s)\n", intn->master->name, intn->master->ifindex, addr);
			inet_ntop(AF_INET, &intn->ipv4_remote, addr, sizeof(addr));
			fprintf(g_conf->stat_file, "  IPv4 Remote            : %s\n", addr);
		}
#endif /* ECMH_BPF */
		inet_ntop(AF_INET6, &intn->linklocal, addr, sizeof(addr));
		fprintf(g_conf->stat_file, "  Link-local address     : %s\n", addr);

		inet_ntop(AF_INET6, &intn->global, addr, sizeof(addr));
		fprintf(g_conf->stat_file, "  Global unicast address : %s\n", addr);

		if (intn->mld_version == 0)
		fprintf(g_conf->stat_file, "  MLD version            : none\n");
		else
		fprintf(g_conf->stat_file, "  MLD version            : v%u\n", intn->mld_version);

		fprintf(g_conf->stat_file, "  Packets received       : %llu\n", intn->stat_packets_received);
		fprintf(g_conf->stat_file, "  Packets sent           : %llu\n", intn->stat_packets_sent);
		fprintf(g_conf->stat_file, "  Bytes received         : %llu\n", intn->stat_bytes_received);
		fprintf(g_conf->stat_file, "  Bytes sent             : %llu\n", intn->stat_bytes_sent);
		fprintf(g_conf->stat_file, "  ICMP's received        : %llu\n", intn->stat_icmp_received);
		fprintf(g_conf->stat_file, "  ICMP's sent            : %llu\n", intn->stat_icmp_sent);
	}

	fprintf(g_conf->stat_file, "\n");
	fprintf(g_conf->stat_file, "*** Interface Dump (end - %u interfaces)\n", g_conf->ints->count);
	fprintf(g_conf->stat_file, "\n");

	/* Dump out some generic program statistics */
	strftime(addr, sizeof(addr), "%Y-%m-%d %H:%M:%S", gmtime(&g_conf->stat_starttime));

	fprintf(g_conf->stat_file, "*** Statistics Dump\n");
	fprintf(g_conf->stat_file, "Version              : ecmh %s\n", ECMH_VERSION);
	fprintf(g_conf->stat_file, "Started              : %s GMT\n", addr);
	fprintf(g_conf->stat_file, "Uptime               : %u days %02u:%02u:%02u\n", uptime_d, uptime_h, uptime_m, uptime_s);
#ifdef ECMH_BPF
	fprintf(g_conf->stat_file, "\n");
	fprintf(g_conf->stat_file, "Tunnelmode           : %s\n", g_conf->tunnelmode ? "Active" : "Disabled");
#endif /* ECMH_BPF */
	fprintf(g_conf->stat_file, "\n");
	fprintf(g_conf->stat_file, "Interfaces Monitored : %u\n", g_conf->ints->count);
	fprintf(g_conf->stat_file, "Groups Managed       : %u\n", g_conf->groups->count);
	fprintf(g_conf->stat_file, "Total Subscriptions  : %u\n", subscriptions);
#ifdef ECMH_SUPPORT_MLD2
	fprintf(g_conf->stat_file, "v2 Robustness Factor : %u\n", ECMH_ROBUSTNESS_FACTOR);
#endif
	fprintf(g_conf->stat_file, "Subscription Timeout : %u\n", ECMH_SUBSCRIPTION_TIMEOUT * ECMH_ROBUSTNESS_FACTOR);
	fprintf(g_conf->stat_file, "\n");
	fprintf(g_conf->stat_file, "Packets Received     : %llu\n", g_conf->stat_packets_received);
	fprintf(g_conf->stat_file, "Packets Sent         : %llu\n", g_conf->stat_packets_sent);
	fprintf(g_conf->stat_file, "Bytes Received       : %llu\n", g_conf->stat_bytes_received);
	fprintf(g_conf->stat_file, "Bytes Sent           : %llu\n", g_conf->stat_bytes_sent);
	fprintf(g_conf->stat_file, "ICMP's received      : %llu\n", g_conf->stat_icmp_received);
	fprintf(g_conf->stat_file, "ICMP's sent          : %llu\n", g_conf->stat_icmp_sent);
	fprintf(g_conf->stat_file, "Hop Limit Exceeded   : %llu\n", g_conf->stat_hlim_exceeded);
	fprintf(g_conf->stat_file, "*** Statistics Dump (end)\n");

	/* Flush the information to disk */
	fflush(g_conf->stat_file);

	dolog(LOG_INFO, "Dumped statistics into %s\n", ECMH_DUMPFILE);

	/* Reset the signal */
	signal(i, &sigusr1);
}

/* Let's tell everybody we are a querier and ask */
/* them which groups they want to receive. */
void send_mld_querys()
{
	struct intnode		*intn;
	struct listnode		*ln, *ln2;
	struct in6_addr		any;

	dolog(LOG_DEBUG, "Sending MLD Queries\n");

	/* We want to know about all the groups */
	memset(&any,0,sizeof(any));

	/* Send MLD query's */
	/* Use listloop2 as the node can disappear in sendpacket() */
	LIST_LOOP2(g_conf->ints, intn, ln, ln2)
	{
#ifndef ECMH_SUPPORT_MLD2
		mld_send_query(intn, &any, NULL);
#else
		mld_send_query(intn, &any, NULL, false);
#endif
	}
	LIST_LOOP2_END

	dolog(LOG_DEBUG, "Sending MLD Queries - done\n");
}

void timeout_signal()
{
	/*
	 * Mark it to be ignored, this avoids double timeouts
	 * one never knows if it takes too long to handle
	 * the first one.
	 */
	signal(SIGALRM, SIG_IGN);
	
	/* Set the needs_timeout */
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

	dolog(LOG_DEBUG, "Timeout\n");

	/* Update the complete interfaces list */
	update_interfaces(NULL);

	/* Get the current time */
	time_tee = time(NULL);

	/* Timeout all the groups that didn't refresh yet */
	LIST_LOOP2(g_conf->groups, groupn, ln, ln2)
	{
		printf("Groups\n");
		LIST_LOOP2(groupn->interfaces, grpintn, gn, gn2)
		{
			printf("Group Interfaces\n");
			LIST_LOOP2(grpintn->subscriptions, subscrn, ssn, ssn2)
			{
				/* Calculate the difference */
				int i = time_tee - subscrn->refreshtime;
				if (i < 0) i = -i;

				/* Dead too long? */
				if (i > (ECMH_SUBSCRIPTION_TIMEOUT * ECMH_ROBUSTNESS_FACTOR))
				{
					/* Dead too long -> delete it */
					list_delete_node(grpintn->subscriptions, ssn);
					/* Destroy the subscription itself */
					subscr_destroy(subscrn);
				}
			}
			LIST_LOOP2_END
#ifndef ECMH_SUPPORT_MLD2
			if (grpintn->subscriptions->count == 0)
#else
			if (grpintn->subscriptions->count <= (-ECMH_ROBUSTNESS_FACTOR))
#endif
			{
				/* Delete from the list */
				list_delete_node(groupn->interfaces, gn);
				/* Destroy the grpint */
				grpint_destroy(grpintn);
			}
		}
		LIST_LOOP2_END

		if (groupn->interfaces->count == 0)
		{
			/* Delete from the list */
			list_delete_node(g_conf->groups, ln);
			/* Destroy the group */
			group_destroy(groupn);
		}
	}
	LIST_LOOP2_END

	/* Send out MLD queries */
	send_mld_querys();

	dolog(LOG_DEBUG, "Timeout - done\n");
}

bool handleinterfaces(uint8_t *buffer)
{
	int			i=0, len;
	struct intnode		*intn = NULL;

#ifndef ECMH_BPF
	struct sockaddr_ll	sa;
	socklen_t		salen;

	salen = sizeof(sa);
	memset(&sa, 0, sizeof(sa));
	len = recvfrom(g_conf->rawsocket, buffer, g_conf->bufferlen, 0, (struct sockaddr *)&sa, &salen);

	if (len == -1)
	{
		dolog(LOG_ERR, "Couldn't Read from RAW Socket\n");
		return false;
	}

	/*
	 * Ignore:
	 * - loopback traffic
	 * - any packets that originate from this host
	 */
	if (	sa.sll_hatype == ARPHRD_LOOPBACK ||
		sa.sll_pkttype == PACKET_OUTGOING)
	{
		return true;
	}

	/* Update statistics */
	g_conf->stat_packets_received++;
	g_conf->stat_bytes_received+=len;

	/* The interface we need to find */
	i = sa.sll_ifindex;

	intn = int_find(i, true);
	if (!intn)
	{
		/* Create a new interface */
		intn = int_create(i);
		if (intn)
		{
			/* Determine linklocal address etc. */
			update_interfaces(intn);

			/* Add it to the list */
			int_add(intn);
		}
	}
	if (intn)
	{
		/* Handle the packet */
		l2_ethtype(intn, buffer, len, ntohs(sa.sll_protocol));
	}
	else
	{
		dolog(LOG_ERR, "Couldn't find interface link %u\n", i);
	}
	return true;
#else /* !ECMH_BPF */
	uint8_t			*bp, *ep, *op, *rbuffer = buffer;
	struct bpf_hdr		*bhp;
	struct ifreq		ifr;
	fd_set			fd_read;
	struct timeval		timeout;
	struct listnode		*ln;

	/* What we want to know */
	memcpy(&fd_read, &g_conf->selectset, sizeof(fd_read));

	memset(&timeout, 0, sizeof(timeout));
	timeout.tv_sec = 5;

	i = select(g_conf->hifd+1, &fd_read, NULL, NULL, &timeout);
	if (i < 0)
	{
		if (errno == EINTR) return true;
		dolog(LOG_ERR, "Select failed\n");
		return false;
	}
	if (i == 0) return true;

	LIST_LOOP(g_conf->ints, intn, ln)
	{
		if (	intn->socket == -1 ||
			!FD_ISSET(intn->socket, &fd_read)) continue;

		len = read(intn->socket, rbuffer, intn->bufferlen);
		if (len < 0)
		{
			dolog(LOG_ERR, "Couldn't read from BPF device: %s (%d)\n", strerror(errno), errno);
			return false;
		}

		bp = buffer = rbuffer;
		bhp = (struct bpf_hdr *)bp;

		for ( ep = bp + bhp->bh_caplen;
			bp < ep;
			bp += BPF_WORDALIGN(bhp->bh_caplen + bhp->bh_hdrlen))
		{
		    	bhp = (struct bpf_hdr *)bp;
		    	buffer = bp + bhp->bh_hdrlen;

			/* Layer 2 packet */
			l2_eth(intn, (struct ether_header *)buffer, bhp->bh_caplen);
		}
	} /* Interfaces */
#endif /* !ECMH_BPF */
	return true;
}

/* Long options */
static struct option const long_options[] = {
	{"foreground",		no_argument,		NULL, 'f'},
	{"user",		required_argument,	NULL, 'u'},
	{"tunnelmode",		no_argument,		NULL, 't'},
	{"notunnelmode",	no_argument,		NULL, 'T'},
	{"verbose",		no_argument,		NULL, 'v'},
	{"version",		no_argument,		NULL, 'V'},
	{NULL,			0, NULL, 0},
};

int main(int argc, char *argv[], char *envp[])
{
	int			i, drop_uid = 0, drop_gid = 0, option_index = 0;
	struct passwd		*passwd;
        struct sched_param      schedparam;
	bool			quit = false;

	init();

	/* Handle arguments */
	while ((i = getopt_long(argc, argv, "fu:tTvV", long_options, &option_index)) != EOF)
	{
		switch (i)
		{
		case 0:
			/* Long option */
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
#ifdef ECMH_BPF
		case 't':
			g_conf->tunnelmode = true;
			break;
		case 'T':
			g_conf->tunnelmode = false;
			break;
#endif

		case 'v':
			g_conf->verbose = true;
			break;

		case 'V':
			printf(ECMH_VERSION_STRING, ECMH_VERSION);
			return 0;

		default:
			fprintf(stderr,
				"%s [-f] [-u username] [-t|-T] [-v] [-V]"
				"\n"
				"\n"
				"-f, --foreground           don't daemonize\n"
				"-u, --user username        drop (setuid+setgid) to user after startup\n"
#ifdef ECMH_BPF
				"-t, --tunnelmode           Don't attach to tunnels, but use proto-41 decapsulation (default)\n"
				"-T, --notunnelmode         Attach to tunnels seperatly\n"
#endif
				"-v, --verbose              Verbose Operation\n"
				"-V, --version              Report version and exit\n"
				
				"\n"
				"Report bugs to Jeroen Massar <jeroen@unfix.org>.\n"
				"Also see the website at http://unfix.org/projects/ecmh/\n",
				argv[0]);
			return -1;
		}
	}

	/* Daemonize */
	if (g_conf->daemonize)
	{
		int i = fork();
		if (i < 0)
		{
			fprintf(stderr, "Couldn't fork\n");
			return -1;
		}
		/* Exit the mother fork */
		if (i != 0) return 0;

		/* Child fork */
		setsid();
		/* Cleanup stdin/out/err */
		freopen("/dev/null","r",stdin);
		freopen("/dev/null","w",stdout);
		freopen("/dev/null","w",stderr);
	}

	/* Handle a SIGHUP to reload the config */
	signal(SIGHUP, &sighup);

	/* Handle SIGTERM/INT/KILL to cleanup the pid file and exit */
	signal(SIGTERM,	&cleanpid);
	signal(SIGINT,	&cleanpid);
	signal(SIGKILL,	&cleanpid);

	/* Timeout handling */
	signal(SIGALRM, &timeout_signal);
	alarm(ECMH_SUBSCRIPTION_TIMEOUT);

	/* Dump operations */
	signal(SIGUSR1,	&sigusr1);

	signal(SIGUSR2, SIG_IGN);

	/* Show our version in the startup logs ;) */
	dolog(LOG_INFO, ECMH_VERSION_STRING, ECMH_VERSION);
#ifdef ECMH_BPF
	dolog(LOG_INFO, "Tunnelmode is %s\n", g_conf->tunnelmode ? "Active" : "Disabled");
#endif

	/* Save our PID */
	savepid();

	/* Open our dump file */
	g_conf->stat_file = fopen(ECMH_DUMPFILE, "w");
	if (!g_conf->stat_file)
	{
		dolog(LOG_ERR, "Couldn't open dumpfile %s\n", ECMH_DUMPFILE);
		return -1;
	}


#ifndef ECMH_BPF
	/*
	 * Allocate a single PACKET socket which can send and receive
	 * anything we want (anything ???.... anythinggg... ;)
	 * This is only available on Linux though
	 */
	g_conf->rawsocket = socket(PF_PACKET, SOCK_DGRAM, htons(ETH_P_ALL));
	if (g_conf->rawsocket < 0)
	{
		dolog(LOG_ERR, "Couldn't allocate a RAW socket\n");
		return -1;
	}

#endif /* ECMH_BPF */

	g_conf->buffer = malloc(g_conf->bufferlen);
	if (!g_conf->buffer)
	{
		dolog(LOG_INFO, "Couldn't allocate memory for buffer: %s (%d)\n", strerror(errno), errno);
		return -1;
	}

	/* Fix our priority, we need to be near realtime */
	if (setpriority(PRIO_PROCESS, getpid(), -15) == -1)
	{
		dolog(LOG_WARNING, "Couldn't raise priority to -15, if streams are shaky, upgrade your cpu or fix this\n");
	}

	/* Change scheduler for higher accuracy */
	memset(&schedparam, 0, sizeof(schedparam));
	schedparam.sched_priority = 99;
	if (sched_setscheduler(0, SCHED_FIFO, &schedparam) == -1)
	{
		dolog(LOG_WARNING, "Couldn't configure the scheduler, errno = %i\n",errno);
	}

	/*
	 * Drop our root priveleges.
	 * We don't need them anymore anyways
	 */
	if (drop_uid != 0) setuid(drop_uid);
	if (drop_gid != 0) setgid(drop_gid);

	/* Update the complete interfaces list */
	update_interfaces(NULL);

	send_mld_querys();

	while (!g_conf->quit && !quit)
	{
		/* Was a timeout set? */
		if (g_needs_timeout)
		{
			/* Run timeout routine */
			timeout();
			
			/* Turn it off */
			g_needs_timeout = false;

			/* Reset the alarm */
			signal(SIGALRM, &timeout_signal);
			alarm(ECMH_SUBSCRIPTION_TIMEOUT);
		}

		quit = !handleinterfaces(g_conf->buffer);
	}

	/* Dump the stats one last time */
	sigusr1(SIGUSR1);

	/* Show the message in the log */
	dolog(LOG_INFO, "Shutdown, thank you for using ecmh\n");

	/* Cleanup the nodes
	 * First the groups, otherwise the references to the
	 * names are gone when we need them when displaying
         * the group deletions from the interfaces ;)
	 */
	list_delete_all_node(g_conf->groups);

#ifdef ECMH_BPF
	/* Clear the locals */
	list_delete_all_node(g_conf->locals);
#endif

	/* Get rid of the interfaces too now */
	list_delete_all_node(g_conf->ints);

	/* Close files and sockets */
	fclose(g_conf->stat_file);
#ifndef ECMH_BPF
	close(g_conf->rawsocket);
#endif

	/* Free the config memory */
	free(g_conf);

	cleanpid(SIGINT);

	return 0;
}
