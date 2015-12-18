/*
 *  ircd-ratbox: A slightly useful ircd.
 *  whowas.c: WHOWAS user cache.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2012 ircd-ratbox development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *
 *  $Id$
 */

#include <stdinc.h>
#include <struct.h>
#include <hash.h>
#include <whowas.h>
#include <match.h>
#include <ircd.h>
#include <numeric.h>
#include <s_serv.h>
#include <s_user.h>
#include <send.h>
#include <s_conf.h>
#include <client.h>
#include <send.h>
#include <s_log.h>

struct whowas_top
{
	char *name;
	rb_dlink_list wwlist;
	hash_node *hnode;
};

static rb_dlink_list *whowas_list;
static unsigned int whowas_list_length = NICKNAMEHISTORYLENGTH;
static void whowas_trim(void *unused);

static void
whowas_free_wtop(struct whowas_top *wtop)
{
	if(rb_dlink_list_length(&wtop->wwlist) == 0)
	{
		hash_del_hnode(HASH_WHOWAS, wtop->hnode);
		rb_free(wtop->name);
		rb_free(wtop);
	}
}

static struct whowas_top *
whowas_get_top(const char *name)
{
	hash_node *hnode;
	struct whowas_top *wtop;

	hnode = hash_find(HASH_WHOWAS, name);
	if(hnode != NULL)
	{
		return (struct whowas_top *) hnode->data;
	}
	wtop = rb_malloc(sizeof(struct whowas_top));
	wtop->name = rb_strdup(name);
	hnode = hash_add(HASH_WHOWAS, name, wtop);
	wtop->hnode = hnode;
	return wtop;
}

rb_dlink_list *
whowas_get_list(const char *name)
{
	struct whowas_top *wtop;
	wtop = hash_find_data(HASH_WHOWAS, name);
	if(wtop == NULL)
		return NULL;
	return &wtop->wwlist;
}

void
whowas_add_history(struct Client *client_p, bool online)
{
	struct whowas_top *wtop;
	whowas_t *who;
	s_assert(NULL != client_p);

	if(client_p == NULL)
		return;

	/* trim some of the entries if we're getting well over our history length */
	if(rb_dlink_list_length(whowas_list) > whowas_list_length + 100)
		whowas_trim(NULL);

	wtop = whowas_get_top(client_p->name);
	who = rb_malloc(sizeof(whowas_t));
	who->wtop = wtop;
	who->logoff = rb_current_time();

	rb_strlcpy(who->name, client_p->name, sizeof(who->name));
	rb_strlcpy(who->username, client_p->username, sizeof(who->username));
	rb_strlcpy(who->hostname, client_p->host, sizeof(who->hostname));
	rb_strlcpy(who->realname, client_p->info, sizeof(who->realname));

	if(MyClient(client_p))
	{
		rb_strlcpy(who->sockhost, client_p->sockhost, sizeof(who->sockhost));
		who->spoof = IsIPSpoof(client_p);
	}
	else
	{
		who->spoof = false;
		if(EmptyString(client_p->sockhost) || !strcmp(client_p->sockhost, "0"))
			who->sockhost[0] = '\0';
		else
			rb_strlcpy(who->sockhost, client_p->sockhost, sizeof(who->sockhost));
	}

	/* this is safe do to with the servername cache */
	who->servername = client_p->servptr->name;

	if(online == true)
	{
		who->online = client_p;
		rb_dlinkAdd(who, &who->cnode, &client_p->whowas_clist);
	}
	else
		who->online = NULL;

	rb_dlinkAdd(who, &who->wnode, &wtop->wwlist);
	rb_dlinkAdd(who, &who->whowas_node, whowas_list);
}


void
whowas_off_history(struct Client *client_p)
{
	rb_dlink_node *ptr, *next;

	RB_DLINK_FOREACH_SAFE(ptr, next, client_p->whowas_clist.head)
	{
		whowas_t *who = ptr->data;
		who->online = NULL;
		rb_dlinkDelete(&who->cnode, &client_p->whowas_clist);
	}
}

struct Client *
whowas_get_history(const char *nick, time_t timelimit)
{
	struct whowas_top *wtop;
	rb_dlink_node *ptr;

	wtop = hash_find_data(HASH_WHOWAS, nick);
	if(wtop == NULL)
		return NULL;

	timelimit = rb_current_time() - timelimit;

	RB_DLINK_FOREACH_PREV(ptr, wtop->wwlist.tail)
	{
		whowas_t *who = ptr->data;
		if(who->logoff >= timelimit)
		{
			return who->online;
		}
	}
	return NULL;;
}

static void
whowas_trim(void *unused)
{
	long over;

	if(rb_dlink_list_length(whowas_list) < whowas_list_length)
		return;
	over = rb_dlink_list_length(whowas_list) - whowas_list_length;

	/* remove whowas entries over the configured length */
	for(long i = 0; i < over; i++)
	{
		if(whowas_list->tail != NULL && whowas_list->tail->data != NULL)
		{
			whowas_t *twho = whowas_list->tail->data;
			if(twho->online != NULL)
				rb_dlinkDelete(&twho->cnode, &twho->online->whowas_clist);
			rb_dlinkDelete(&twho->wnode, &twho->wtop->wwlist);
			rb_dlinkDelete(&twho->whowas_node, whowas_list);
			whowas_free_wtop(twho->wtop);
			rb_free(twho);
		}
	}
}

void
whowas_init(void)
{
	whowas_list = rb_malloc(sizeof(rb_dlink_list));
	if(whowas_list_length == 0)
	{
		whowas_list_length = NICKNAMEHISTORYLENGTH;
	}
	rb_event_add("whowas_trim", whowas_trim, NULL, 10);
}

void
whowas_set_size(int len)
{
	whowas_list_length = len;
	whowas_trim(NULL);
}

void
whowas_memory_usage(size_t * count, size_t * memused)
{
	size_t hcount;
	hash_get_memusage(HASH_WHOWAS, &hcount, memused);
	*count = rb_dlink_list_length(whowas_list);
	*memused += *count * sizeof(whowas_t);
	*memused += sizeof(struct whowas_top) * hcount;
}
