/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_accept.c: Allows a user to talk to a +g user.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 *
 *  $Id$
 */

#include "stdinc.h"
#include "tools.h"
#include "struct.h"
#include "client.h"
#include "ircd.h"
#include "numeric.h"
#include "s_conf.h"
#include "send.h"
#include "parse.h"
#include "snprintf.h"
#include "modules.h"
#include "irc_string.h"

static int m_accept(struct Client *, struct Client *, int, const char **);
static void build_nicklist(struct Client *, char *, char *, const char *);

static void add_accept(struct Client *, struct Client *);
static void list_accepts(struct Client *);

struct Message accept_msgtab = {
	"ACCEPT", 0, 0, 0, MFLG_SLOW | MFLG_UNREG,
	{mg_unreg, {m_accept, 2}, mg_ignore, mg_ignore, mg_ignore, {m_accept, 2}}
};

mapi_clist_av1 accept_clist[] = {
	&accept_msgtab, NULL
};
DECLARE_MODULE_AV1(accept, NULL, NULL, accept_clist, NULL, NULL, "$Revision: 19295 $");

/*
 * m_accept - ACCEPT command handler
 *      parv[0] = sender prefix
 *      parv[1] = servername
 */
static int
m_accept(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	char *nick;
	char *p = NULL;
	static char addbuf[BUFSIZE];
	static char delbuf[BUFSIZE];
	struct Client *target_p;
	int accept_num;

	if(*parv[1] == '*')
	{
		list_accepts(source_p);
		return 0;
	}

	build_nicklist(source_p, addbuf, delbuf, parv[1]);

	/* parse the delete list */
	for (nick = strtoken(&p, delbuf, ","); nick != NULL; nick = strtoken(&p, NULL, ","))
	{
		/* shouldnt happen, but lets be paranoid */
		if((target_p = find_named_person(nick)) == NULL)
		{
			sendto_one_numeric(source_p, POP_QUEUE, ERR_NOSUCHNICK,
					   form_str(ERR_NOSUCHNICK), nick);
			continue;
		}

		/* user isnt on clients accept list */
		if(!accept_message(target_p, source_p))
		{
			sendto_one(source_p, POP_QUEUE, form_str(ERR_ACCEPTNOT),
				   me.name, source_p->name, target_p->name);
			continue;
		}

                dlinkFindDestroy(target_p, &source_p->localClient->allow_list);
                dlinkFindDestroy(source_p, &target_p->on_allow_list);

	}

	/* get the number of accepts they have */
	accept_num = dlink_list_length(&source_p->localClient->allow_list);

	/* parse the add list */
	for (nick = strtoken(&p, addbuf, ","); nick; nick = strtoken(&p, NULL, ","), accept_num++)
	{
		/* shouldnt happen, but lets be paranoid */
		if((target_p = find_named_person(nick)) == NULL)
		{
			sendto_one_numeric(source_p, POP_QUEUE, ERR_NOSUCHNICK,
					   form_str(ERR_NOSUCHNICK), nick);
			continue;
		}

		/* user is already on clients accept list */
		if(accept_message(target_p, source_p))
		{
			sendto_one(source_p, POP_QUEUE, form_str(ERR_ACCEPTEXIST),
				   me.name, source_p->name, target_p->name);
			continue;
		}

		if(accept_num >= ConfigFileEntry.max_accept)
		{
			sendto_one(source_p, POP_QUEUE, form_str(ERR_ACCEPTFULL), me.name, source_p->name);
			return 0;
		}

		/* why is this here? */
		/* del_from accept(target_p, source_p); */
		add_accept(source_p, target_p);

	}

	return 0;
}

/*
 * build_nicklist()
 *
 * input	- pointer to client
 *		- pointer to addbuffer
 *		- pointer to remove buffer
 *		- pointer to list of nicks
 * output	- 
 * side effects - addbuf/delbuf are modified to give valid nicks
 */
static void
build_nicklist(struct Client *source_p, char *addbuf, char *delbuf, const char *nicks)
{
	char *name;
	char *p;
	int lenadd;
	int lendel;
	int del;
	struct Client *target_p;
	char *n = LOCAL_COPY(nicks);

	*addbuf = *delbuf = '\0';
	del = lenadd = lendel = 0;

	/* build list of clients to add into addbuf, clients to remove in delbuf */
	for (name = strtoken(&p, n, ","); name; name = strtoken(&p, NULL, ","), del = 0)
	{
		if(*name == '-')
		{
			del = 1;
			name++;
		}

		if((target_p = find_named_person(name)) == NULL)
		{
			sendto_one_numeric(source_p, POP_QUEUE, ERR_NOSUCHNICK, 
					   form_str(ERR_NOSUCHNICK), name);
			continue;
		}

		/* we're deleting a client */
		if(del)
		{
			if(*delbuf)
				(void) strcat(delbuf, ",");

			(void) strncat(delbuf, name, BUFSIZE - lendel - 1);
			lendel += strlen(name) + 1;
		}
		/* adding a client */
		else
		{
			if(*addbuf)
				(void) strcat(addbuf, ",");

			(void) strncat(addbuf, name, BUFSIZE - lenadd - 1);
			lenadd += strlen(name) + 1;
		}
	}
}

/*
 * add_accept()
 *
 * input	- pointer to clients accept list to add to
 * 		- pointer to client to add
 * output	- none
 * side effects - target is added to clients list
 */
static void
add_accept(struct Client *source_p, struct Client *target_p)
{
	dlinkAddAlloc(target_p, &source_p->localClient->allow_list);
	dlinkAddAlloc(source_p, &target_p->on_allow_list);
}


/*
 * list_accepts()
 *
 * input 	- pointer to client
 * output	- none
 * side effects	- print accept list to client
 */
static void
list_accepts(struct Client *source_p)
{
	dlink_node *ptr;
	struct Client *target_p;
	char nicks[BUFSIZE];
	int len = 0;
	int len2 = 0;
	int count = 0;

	*nicks = '\0';
	len2 = strlen(source_p->name) + 10;

	DLINK_FOREACH(ptr, source_p->localClient->allow_list.head)
	{
		target_p = ptr->data;

		if(target_p)
		{

			if((len + strlen(target_p->name) + len2 > BUFSIZE) || count > 14)
			{
				sendto_one(source_p, HOLD_QUEUE, form_str(RPL_ACCEPTLIST),
					   me.name, source_p->name, nicks);

				len = count = 0;
				*nicks = '\0';
			}

			len += ircsnprintf(nicks + len, sizeof(nicks) - len, "%s ", target_p->name);
			count++;
		}
	}

	if(*nicks)
		sendto_one(source_p, HOLD_QUEUE, form_str(RPL_ACCEPTLIST), 
			   me.name, source_p->name, nicks);

	sendto_one(source_p, POP_QUEUE, form_str(RPL_ENDOFACCEPT), 
		   me.name, source_p->name);

}
