/*
 * Copyright 2014-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * ctl.c -- implementation of the interface for examining and modification of
 *	the library internal state
 */

#include <sys/param.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>

#include "libpmem.h"
#include "libpmemobj.h"

#include "util.h"
#include "out.h"
#include "lane.h"
#include "redo.h"
#include "memops.h"
#include "pmalloc.h"
#include "heap_layout.h"
#include "list.h"
#include "cuckoo.h"
#include "ctree.h"
#include "obj.h"
#include "sync.h"
#include "valgrind_internal.h"
#include "ctl.h"
#include "memblock.h"
#include "heap.h"

#define CTL_MAX_ENTRIES 100

#define CTL_STRING_QUERY_SEPARATOR ";"
#define CTL_NAME_VALUE_SEPARATOR "="
#define CTL_QUERY_NODE_SEPARATOR "."

/*
 * This is the top level node of the ctl tree structure. Each node can contain
 * children and leaf nodes.
 *
 * Internal nodes simply create a new path in the tree whereas child nodes are
 * the ones providing the read/write functionality by the means of callbacks.
 *
 * Each tree node must be NULL-terminated, CTL_NODE_END macro is provided for
 * convience.
 */
struct ctl {
	struct ctl_node root[CTL_MAX_ENTRIES];
	int first_free;
};

/*
 * String provider is the simplest, elementary, query provider. It can be used
 * directly to parse environment variables or in conjuction with other code to
 * provide more complex behavior. It is initialized with a string containing all
 * of the queries and tokenizes it into separate structures.
 */
struct ctl_string_provider {
	struct ctl_query_provider super;

	char *buf; /* stores the entire string that needs to be parsed */
	char *sptr; /* for internal use of strtok */
};

/*
 * ctl_query -- (internal) parses the name and calls the appropriate methods
 *	from the ctl tree.
 */
static int
ctl_query(PMEMobjpool *pop, enum ctl_query_type type,
	const char *name, void *read_arg, void *write_arg)
{
	struct ctl_node *nodes = pop->ctl->root;
	struct ctl_node *n = NULL;

	/*
	 * All of the indexes are put on this list so that the handlers can
	 * easily retrieve the index values. The list is cleared once the ctl
	 * query has been handled.
	 */
	struct ctl_indexes indexes;
	SLIST_INIT(&indexes);

	int ret = -1;

	char *parse_str = Strdup(name);
	if (parse_str == NULL)
		goto error_strdup_name;

	char *sptr = NULL;
	char *node_name = strtok_r(parse_str, CTL_QUERY_NODE_SEPARATOR, &sptr);

	/*
	 * Go through the string and separate tokens that correspond to nodes
	 * in the main ctl tree.
	 */
	while (node_name != NULL) {
		char *endptr;
		long index_value = strtol(node_name, &endptr, 0);
		struct ctl_index *index_entry = NULL;
		if (endptr != node_name) { /* a valid index */
			index_entry = Malloc(sizeof(*index_entry));
			if (index_entry == NULL)
				goto error_invalid_arguments;
			index_entry->value = index_value;
			SLIST_INSERT_HEAD(&indexes, index_entry, entry);
		}

		for (n = &nodes[0]; n->name != NULL; ++n) {
			if (index_entry && n->type == CTL_NODE_INDEXED)
				break;
			else if (strcmp(n->name, node_name) == 0)
				break;
		}
		if (n->name == NULL) {
			errno = EINVAL;
			goto error_invalid_arguments;
		}
		if (index_entry)
			index_entry->name = n->name;

		nodes = n->children;
		node_name = strtok_r(NULL, CTL_QUERY_NODE_SEPARATOR, &sptr);
	}

	/*
	 * Discard invalid calls, this includes the ones that are mostly correct
	 * but include an extraneous arguments.
	 */
	if (n == NULL || (read_arg != NULL && n->read_cb == NULL) ||
		(write_arg != NULL && n->write_cb == NULL) ||
		(write_arg == NULL && read_arg == NULL)) {
		errno = EINVAL;
		goto error_invalid_arguments;
	}

	ASSERTeq(n->type, CTL_NODE_LEAF);

	ret = 0;

	if (read_arg)
		ret = n->read_cb(pop, type, read_arg, &indexes);

	if (write_arg && ret == 0)
		ret = n->write_cb(pop, type, write_arg, &indexes);

error_invalid_arguments:
	while (!SLIST_EMPTY(&indexes)) {
		struct ctl_index *index = SLIST_FIRST(&indexes);
		SLIST_REMOVE_HEAD(&indexes, entry);
		Free(index);
	}

	Free(parse_str);

error_strdup_name:
	return ret;
}

/*
 * pmemobj_ctl -- programmatically executes a ctl query
 */
int
pmemobj_ctl(PMEMobjpool *pop, const char *name, void *read_arg, void *write_arg)
{
	return ctl_query(pop, CTL_QUERY_PROGRAMMATIC,
		name, read_arg, write_arg);
}

/*
 * ctl_register_module_node -- adds a new node to the CTL tree root.
 */
void
ctl_register_module_node(struct ctl *c, const char *name, struct ctl_node *n)
{
	struct ctl_node *nnode = &c->root[c->first_free++];
	nnode->children = n;
	nnode->type = CTL_NODE_NAMED;
	nnode->name = Strdup(name);
}

/*
 * ctl_exec_query_config -- (internal) executes a ctl query from a provider
 */
static int
ctl_exec_query_config(PMEMobjpool *pop, struct ctl_query_config *q)
{
	return ctl_query(pop, CTL_QUERY_CONFIG_INPUT, q->name, NULL, q->value);
}

/*
 * ctl_load_config -- executes the entire query collection from a provider
 */
int
ctl_load_config(PMEMobjpool *pop, struct ctl_query_provider *p)
{
	int r = 0;

	struct ctl_query_config q = {NULL, NULL};

	for (r = p->first(p, &q); r == 0; r = p->next(p, &q))
		if ((r = ctl_exec_query_config(pop, &q)) != 0)
			break;

	/* the method 'next' from data provider returns 1 to indicate end */
	return r >= 0 ? 0 : -1;
}

/*
 * ctl_string_provider_parse_query -- (internal) splits an entire query string
 *	into name and value
 */
static int
ctl_string_provider_parse_query(char *qbuf, struct ctl_query_config *q)
{
	if (qbuf == NULL)
		return 1;

	char *sptr;
	q->name = strtok_r(qbuf, CTL_NAME_VALUE_SEPARATOR, &sptr);
	if (q->name == NULL)
		return -1;

	q->value = strtok_r(NULL, CTL_NAME_VALUE_SEPARATOR, &sptr);
	if (q->value == NULL)
		return -1;

	/* the value itself mustn't include CTL_NAME_VALUE_SEPARATOR */
	char *extra = strtok_r(NULL, CTL_NAME_VALUE_SEPARATOR, &sptr);
	if (extra != NULL)
		return -1;

	return 0;
}

/*
 * ctl_string_provider_first -- (internal) returns the first query from the
 *	provider's collection
 */
static int
ctl_string_provider_first(struct ctl_query_provider *p,
	struct ctl_query_config *q)
{
	struct ctl_string_provider *sp = (struct ctl_string_provider *)p;

	char *qbuf = strtok_r(sp->buf, CTL_STRING_QUERY_SEPARATOR, &sp->sptr);

	return ctl_string_provider_parse_query(qbuf, q);
}

/*
 * ctl_string_provider_first -- (internal) returns the next in sequence query
 *	from the provider's collection
 */
static int
ctl_string_provider_next(struct ctl_query_provider *p,
	struct ctl_query_config *q)
{
	struct ctl_string_provider *sp = (struct ctl_string_provider *)p;

	char *qbuf = strtok_r(NULL, CTL_STRING_QUERY_SEPARATOR, &sp->sptr);

	return ctl_string_provider_parse_query(qbuf, q);
}

/*
 * ctl_string_provider_new --
 *	creates and initializes a new string query provider
 */
struct ctl_query_provider *
ctl_string_provider_new(const char *buf)
{
	struct ctl_string_provider *sp =
		Malloc(sizeof(struct ctl_string_provider));
	if (sp == NULL)
		goto error_provider_alloc;

	sp->super.first = ctl_string_provider_first;
	sp->super.next = ctl_string_provider_next;
	sp->buf = Strdup(buf);
	if (sp->buf == NULL)
		goto error_buf_alloc;

	return &sp->super;

error_buf_alloc:
	Free(sp);
error_provider_alloc:
	return NULL;
}

/*
 * ctl_string_provider_delete -- cleanups and deallocates provider instance
 */
void
ctl_string_provider_delete(struct ctl_query_provider *p)
{
	struct ctl_string_provider *sp = (struct ctl_string_provider *)p;
	Free(sp->buf);
	Free(p);
}

/*
 * ctl_new -- allocates and initalizes ctl data structures
 */
struct ctl *
ctl_new(void)
{
	struct ctl *c = Zalloc(sizeof(struct ctl));
	c->first_free = 0;

	return c;
}

/*
 * ctl_delete -- deletes statistics
 */
void
ctl_delete(struct ctl *c)
{
	for (struct ctl_node *n = c->root; n->name != NULL; ++n)
		Free(n->name);

	Free(c);
}
