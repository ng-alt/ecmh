/**************************************
 ecmh - Easy Cast du Multi Hub
 by Jeroen Massar <jeroen@unfix.org>
***************************************
 $Author: fuzzel $
 $Id: groups.h,v 1.2 2004/01/11 21:41:05 fuzzel Exp $
 $Date: 2004/01/11 21:41:05 $
**************************************/

// The node used to hold the groups we joined
struct groupnode
{
	struct in6_addr	mca;			// The Multicast IPv6 address (group)
	struct list	*interfaces;		// The list of grpint nodes (interfaces) that
						// are interrested in this node
	time_t		lastforward;		// The last time we forwarded a report for this group
};

struct groupnode *group_create(const struct in6_addr *mca);
void group_destroy(struct groupnode *groupn);
struct groupnode *group_find(const struct in6_addr *mca);
struct grpintnode *groupint_get(const struct in6_addr *mca, struct intnode *interface);
