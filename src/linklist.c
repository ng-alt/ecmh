/* Generic linked list routine.
 * Copyright (C) 1997, 2000 Kunihiro Ishiguro
 * modded for ecmh by Jeroen Massar
 */

#include "linklist.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>

/* Allocate new list. */
struct list *list_new()
{
	struct list *new;

	new = calloc(1, sizeof(*new));
	if (!new) return NULL;
	return new;
}

/* Free list. */
void list_free(struct list *l)
{
	if (l) free(l);
}

/* Allocate new listnode */
static struct listnode *listnode_new(void);
static struct listnode *listnode_new(void)
{
	struct listnode *node;

	node = calloc(1, sizeof(*node));
	if (!node) return NULL;
	return node;
}

/* Free listnode. */
static void listnode_free(struct listnode *node);
static void listnode_free(struct listnode *node)
{
	free(node);
}

/* Add new data to the list. */
void listnode_add(struct list *list, void *val)
{
	struct listnode *node;

	node = listnode_new();
	if (!node) return;

	node->prev = list->tail;
	node->data = val;

	if (list->head == NULL) list->head = node;
	else list->tail->next = node;
	list->tail = node;

	if (list->count < 0) list->count = 0;
	list->count++;
}

/* Delete all listnode from the list. */
void list_delete_all_node(struct list *list)
{
	struct listnode *node;
	struct listnode *next;

	for (node = list->head; node; node = next)
	{
		next = node->next;
		if (list->del) (*list->del)(node->data);
		listnode_free(node);
	}
	list->head = list->tail = NULL;
	list->count = 0;
}

/* Delete all listnode then free list itself. */
void list_delete(struct list *list)
{
	struct listnode *node;
	struct listnode *next;

	for (node = list->head; node; node = next)
	{
		next = node->next;
		if (list->del) (*list->del)(node->data);
		listnode_free(node);
	}
	list_free (list);
}

/* Delete the node from list.  For ospfd and ospf6d. */
void list_delete_node(struct list *list, struct listnode *node)
{
	if (node->prev) node->prev->next = node->next;
	else list->head = node->next;
	if (node->next) node->next->prev = node->prev;
	else list->tail = node->prev;
	list->count--;
	listnode_free(node);
}

