/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_invite.c: Invites the user to join a channel.
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
#include <channel.h>
#include <client.h>
#include <hash.h>
#include <match.h>
#include <ircd.h>
#include <numeric.h>
#include <send.h>
#include <s_conf.h>
#include <parse.h>
#include <modules.h>

static int m_invite(struct Client *, struct Client *, int, const char **);


struct Message invite_msgtab = {
	.cmd = "INVITE",

	.handlers[UNREGISTERED_HANDLER] =	{ mm_unreg },
	.handlers[CLIENT_HANDLER] =		{ .handler = m_invite, .min_para = 3 },
	.handlers[RCLIENT_HANDLER] =		{ .handler = m_invite, .min_para = 3 },
	.handlers[SERVER_HANDLER] =		{  mm_ignore },
	.handlers[ENCAP_HANDLER] =		{  mm_ignore },
	.handlers[OPER_HANDLER] =		{ .handler = m_invite, .min_para = 3 },
};
mapi_clist_av1 invite_clist[] = { &invite_msgtab, NULL };

DECLARE_MODULE_AV1(invite, NULL, NULL, invite_clist, NULL, NULL, "$Revision$");

static void add_invite(struct Channel *, struct Client *);

/* m_invite()
 *	parv[0] - sender prefix
 *	parv[1] - user to invite
 *	parv[2] - channel name
 */
static int
m_invite(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	struct Client *target_p;
	struct Channel *chptr;
	struct membership *msptr;
	int store_invite = 0;

	if(MyClient(source_p) && !IsFloodDone(source_p))
		flood_endgrace(source_p);

	if(MyClient(source_p))
		target_p = find_named_person(parv[1]);
	else
		target_p = find_person(parv[1]);
	if(target_p == NULL)
	{
		sendto_one_numeric(source_p, ERR_NOSUCHNICK,
				   form_str(ERR_NOSUCHNICK), IsDigit(parv[1][0]) ? "*" : parv[1]);
		return 0;
	}

	if(check_channel_name(parv[2]) == 0)
	{
		sendto_one_numeric(source_p, ERR_BADCHANNAME, form_str(ERR_BADCHANNAME), parv[2]);
		return 0;
	}

	if(!IsChannelName(parv[2]))
	{
		if(MyClient(source_p))
			sendto_one_numeric(source_p, ERR_NOSUCHCHANNEL,
					   form_str(ERR_NOSUCHCHANNEL), parv[2]);
		return 0;
	}

	/* Do not send local channel invites to users if they are not on the
	 * same server as the person sending the INVITE message. 
	 */
	if(parv[2][0] == '&' && !MyConnect(target_p))
	{
		sendto_one_numeric(source_p, s_RPL(ERR_USERNOTONSERV), target_p->name);
		return 0;
	}

	if((chptr = find_channel(parv[2])) == NULL)
	{
		sendto_one_numeric(source_p, s_RPL(ERR_NOSUCHCHANNEL), parv[2]);
		return 0;
	}

	msptr = find_channel_membership(chptr, source_p);
	if(MyClient(source_p) && (msptr == NULL))
	{
		sendto_one_numeric(source_p, ERR_NOTONCHANNEL, form_str(ERR_NOTONCHANNEL), parv[2]);
		return 0;
	}

	if(IsMember(target_p, chptr))
	{
		sendto_one_numeric(source_p, ERR_USERONCHANNEL,
				   form_str(ERR_USERONCHANNEL), target_p->name, parv[2]);
		return 0;
	}

	/* only store invites for +i channels */
	if(ConfigChannel.invite_ops_only || (chptr->mode.mode & MODE_INVITEONLY))
	{
		/* treat remote clients as chanops */
		if(MyClient(source_p) && !is_chanop(msptr))
		{
			sendto_one_numeric(source_p, s_RPL(ERR_CHANOPRIVSNEEDED), parv[2]);
			return 0;
		}

		if(chptr->mode.mode & MODE_INVITEONLY)
			store_invite = 1;
	}

	if(MyConnect(source_p))
	{
		sendto_one_numeric(source_p, s_RPL(RPL_INVITING),
			   target_p->name, parv[2]);
		if(target_p->user->away)
			sendto_one_numeric(source_p, s_RPL(RPL_AWAY),
					   target_p->name, target_p->user->away);
	}

	if(MyConnect(target_p))
	{
		sendto_one(target_p, ":%s!%s@%s INVITE %s :%s",
			   source_p->name, source_p->username, source_p->host,
			   target_p->name, chptr->chname);

		if(store_invite)
			add_invite(chptr, target_p);
	}
	else if(target_p->from != client_p)
	{
		sendto_one_prefix(target_p, source_p, "INVITE", ":%s", chptr->chname);
	}

	return 0;
}

/* add_invite()
 *
 * input	- channel to add invite to, client to add
 * output	-
 * side effects - client is added to invite list.
 */
static void
add_invite(struct Channel *chptr, struct Client *who)
{
	/* already invited? */
	if(rb_dlinkFind(chptr, &who->localClient->invited) != NULL)
	        return;

	/* ok, if their invite list is too long, remove the tail */
	if(rb_dlink_list_length(&who->localClient->invited) >= ConfigChannel.max_chans_per_user)
	{
		rb_dlink_node *ptr = who->localClient->invited.tail;
		del_invite(ptr->data, who);
	}

	/* add user to channel invite list */
	rb_dlinkAddAlloc(who, &chptr->invites);

	/* add channel to user invite list */
	rb_dlinkAddAlloc(chptr, &who->localClient->invited);
}
