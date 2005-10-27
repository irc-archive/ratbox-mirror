/*
 *  ircd-ratbox: A slightly useful ircd.
 *  parse.c: The message parser.
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
#include "struct.h"
#include "parse.h"
#include "client.h"
#include "channel.h"
#include "hash.h"
#include "match.h"
#include "ircd.h"
#include "numeric.h"
#include "s_log.h"
#include "s_stats.h"
#include "send.h"
#include "s_conf.h"
#include "s_serv.h"

/*
 * NOTE: parse() should not be called recursively by other functions!
 */
static char *sender;

/* parv[0] == source, and parv[LAST] == NULL */
static char *para[MAXPARA + 2];

static void cancel_clients(struct Client *, struct Client *, char *);
static void remove_unknown(struct Client *, char *, char *);

static void do_numeric(char[], struct Client *, struct Client *, int, char **);

static int handle_command(struct Message *, struct Client *, struct Client *, int, const char**);

static int cmd_hash(const char *p);
static struct Message *hash_parse(const char *);

#define MAX_MSG_HASH 512 /* don't change this unless you know what you are doing */
struct MessageHash *msg_hash_table[MAX_MSG_HASH];

static char buffer[1024];

/* turn a string into a parc/parv pair */


inline int
string_to_array(char *string, char **parv)
{
	char *p, *buf = string;
	int x = 1;

	parv[x] = NULL;
	while (*buf == ' ')	/* skip leading spaces */
		buf++;
	if(*buf == '\0')	/* ignore all-space args */
		return x;

	do
	{
		if(*buf == ':')	/* Last parameter */
		{
			buf++;
			parv[x++] = buf;
			parv[x] = NULL;
			return x;
		}
		else
		{
			parv[x++] = buf;
			parv[x] = NULL;
			if((p = strchr(buf, ' ')) != NULL)
			{
				*p++ = '\0';
				buf = p;
			}
			else
				return x;
		}
		while (*buf == ' ')
			buf++;
		if(*buf == '\0')
			return x;
	}
	/* we can go upto parv[MAXPARA], as parv[0] is taken by source */
	while (x < MAXPARA);

	if(*p == ':')
		p++;

	parv[x++] = p;
	parv[x] = NULL;
	return x;
}

/* parse()
 *
 * given a raw buffer, parses it and generates parv, parc and sender
 */
void
parse(struct Client *client_p, char *pbuffer, char *bufend)
{
	struct Client *from = client_p;
	char *ch;
	char *s;
	char *end;
	int i = 1;
	char *numeric = 0;
	struct Message *mptr;

	s_assert(MyConnect(client_p));
	s_assert(client_p->localClient->fd >= 0);
	if(IsAnyDead(client_p))
		return;

	for (ch = pbuffer; *ch == ' '; ch++)	/* skip spaces */
		/* null statement */ ;

	para[0] = from->name;

	if(*ch == ':')
	{
		ch++;

		/* point sender to the sender param */
		sender = ch;

		if((s = strchr(ch, ' ')))
		{
			*s = '\0';
			s++;
			ch = s;
		}

		if(*sender && IsServer(client_p))
		{
			from = find_any_client(sender);

			/* didnt find any matching client, issue a kill */
			if(from == NULL)
			{
				ServerStats.is_unpf++;
				remove_unknown(client_p, sender, pbuffer);
				return;
			}

			para[0] = from->name;

			/* fake direction, hmm. */
			if(from->from != client_p)
			{
				ServerStats.is_wrdi++;
				cancel_clients(client_p, from, pbuffer);
				return;
			}
		}
		while (*ch == ' ')
			ch++;
	}

	if(*ch == '\0')
	{
		ServerStats.is_empt++;
		return;
	}

	/* at this point there must be some sort of command parameter */

	/*
	 * Extract the command code from the packet.  Point s to the end
	 * of the command code and calculate the length using pointer
	 * arithmetic.  Note: only need length for numerics and *all*
	 * numerics must have parameters and thus a space after the command
	 * code. -avalon
	 */

	/* EOB is 3 chars long but is not a numeric */

	if(*(ch + 3) == ' ' &&	/* ok, lets see if its a possible numeric.. */
	   IsDigit(*ch) && IsDigit(*(ch + 1)) && IsDigit(*(ch + 2)))
	{
		mptr = NULL;
		numeric = ch;
		ServerStats.is_num++;
		s = ch + 3;	/* I know this is ' ' from above if */
		*s++ = '\0';	/* blow away the ' ', and point s to next part */
	}
	else
	{
		int ii = 0;

		if((s = strchr(ch, ' ')))
			*s++ = '\0';

		mptr = hash_parse(ch);

		/* no command or its encap only, error */
		if(!mptr || !mptr->cmd)
		{
			/*
			 * Note: Give error message *only* to recognized
			 * persons. It's a nightmare situation to have
			 * two programs sending "Unknown command"'s or
			 * equivalent to each other at full blast....
			 * If it has got to person state, it at least
			 * seems to be well behaving. Perhaps this message
			 * should never be generated, though...  --msa
			 * Hm, when is the buffer empty -- if a command
			 * code has been found ?? -Armin
			 */
			if(pbuffer[0] != '\0')
			{
				if(IsPerson(from))
					sendto_one(from, POP_QUEUE, form_str(ERR_UNKNOWNCOMMAND),
						   me.name, from->name, ch);
			}
			ServerStats.is_unco++;
			return;
		}

		ii = bufend - ((s) ? s : ch);
		mptr->bytes += ii;
	}

	end = bufend - 1;

	/* XXX this should be done before parse() is called */
	if(*end == '\n')
		*end-- = '\0';
	if(*end == '\r')
		*end = '\0';

	if(s != NULL)
		i = string_to_array(s, para);

	if(mptr == NULL)
	{
		do_numeric(numeric, client_p, from, i, para);
		return;
	}

	if(handle_command(mptr, client_p, from, i, /* XXX discards const!!! */ (const char **)para) < -1)
	{
		char *p;
		for (p = pbuffer; p <= end; p += 8)
		{
			/* HACK HACK */
			/* Its expected this nasty code can be removed
			 * or rewritten later if still needed.
			 */
			if((unsigned long) (p + 8) > (unsigned long) end)
			{
				for (; p <= end; p++)
				{
					ilog(L_MAIN, "%02x |%c", p[0], p[0]);
				}
			}
			else
				ilog(L_MAIN,
				     "%02x %02x %02x %02x %02x %02x %02x %02x |%c%c%c%c%c%c%c%c",
				     p[0], p[1], p[2], p[3], p[4], p[5],
				     p[6], p[7], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
		}
	}

}

/*
 * handle_command
 *
 * inputs	- pointer to message block
 *		- pointer to client
 *		- pointer to client message is from
 *		- count of number of args
 *		- pointer to argv[] array
 * output	- -1 if error from server
 * side effects	-
 */
static int
handle_command(struct Message *mptr, struct Client *client_p,
	       struct Client *from, int i, const char** hpara)
{
	struct MessageEntry ehandler;
	MessageHandler handler = 0;

	if(IsAnyDead(client_p))
		return -1;

	if(IsServer(client_p))
		mptr->rcount++;

	mptr->count++;

	/* New patch to avoid server flooding from unregistered connects
	   - Pie-Man 07/27/2000 */

	if(!IsRegistered(client_p))
	{
		/* if its from a possible server connection
		 * ignore it.. more than likely its a header thats sneaked through
		 */

		if(IsAnyServer(client_p) && !(mptr->flags & MFLG_UNREG))
			return (1);
	}

	ehandler = mptr->handlers[from->handler];
	handler = ehandler.handler;

	/* check right amount of params is passed... --is */
	if(i < ehandler.min_para || 
	   (ehandler.min_para && EmptyString(hpara[ehandler.min_para - 1])))
	{
		if(!IsServer(client_p))
		{
			sendto_one(client_p, POP_QUEUE, form_str(ERR_NEEDMOREPARAMS),
				   me.name, 
				   EmptyString(client_p->name) ? "*" : client_p->name, 
				   mptr->cmd);
			if(MyClient(client_p))
				return (1);
			else
				return (-1);
		}

		sendto_realops_flags(UMODE_ALL, L_ALL,
				     "Dropping server %s due to (invalid) command '%s'"
				     " with only %d arguments (expecting %d).",
				     client_p->name, mptr->cmd, i, ehandler.min_para);
		ilog(L_SERVER,
		     "Insufficient parameters (%d) for command '%s' from %s.",
		     i, mptr->cmd, client_p->name);

		exit_client(client_p, client_p, client_p,
			    "Not enough arguments to server command.");
		return (-1);
	}

	(*handler) (client_p, from, i, hpara);
	return (1);
}

void
handle_encap(struct Client *client_p, struct Client *source_p,
	     const char *command, int parc, const char *parv[])
{
	struct Message *mptr;
	struct MessageEntry ehandler;
	MessageHandler handler = 0;

	parv[0] = source_p->name;

	mptr = hash_parse(command);

	if(mptr == NULL || mptr->cmd == NULL)
		return;

	ehandler = mptr->handlers[ENCAP_HANDLER];
	handler = ehandler.handler;

	if(parc < ehandler.min_para || 
	   (ehandler.min_para && EmptyString(parv[ehandler.min_para - 1])))
		return;

	(*handler) (client_p, source_p, parc, parv);
}

/*
 * clear_hash_parse()
 *
 * inputs       -
 * output       - NONE
 * side effects - MUST MUST be called at startup ONCE before
 *                any other keyword hash routine is used.
 *
 */
void
clear_hash_parse()
{
	memset(msg_hash_table, 0, sizeof(msg_hash_table));
}

/* mod_add_cmd
 *
 * inputs	- command name
 *		- pointer to struct Message
 * output	- none
 * side effects - load this one command name
 *		  msg->count msg->bytes is modified in place, in
 *		  modules address space. Might not want to do that...
 */
void
mod_add_cmd(struct Message *msg)
{
	struct MessageHash *ptr;
	struct MessageHash *last_ptr = NULL;
	struct MessageHash *new_ptr;
	int msgindex;

	s_assert(msg != NULL);
	if(msg == NULL)
		return;

	msgindex = cmd_hash(msg->cmd);

	for (ptr = msg_hash_table[msgindex]; ptr; ptr = ptr->next)
	{
		if(strcasecmp(msg->cmd, ptr->cmd) == 0)
			return;	/* Its already added */
		last_ptr = ptr;
	}

	new_ptr = (struct MessageHash *) ircd_malloc(sizeof(struct MessageHash));

	new_ptr->next = NULL;
	DupString(new_ptr->cmd, msg->cmd);
	new_ptr->msg = msg;

	msg->count = 0;
	msg->rcount = 0;
	msg->bytes = 0;

	if(last_ptr == NULL)
		msg_hash_table[msgindex] = new_ptr;
	else
		last_ptr->next = new_ptr;
}

/* mod_del_cmd
 *
 * inputs	- command name
 * output	- none
 * side effects - unload this one command name
 */
void
mod_del_cmd(struct Message *msg)
{
	struct MessageHash *ptr;
	struct MessageHash *last_ptr = NULL;
	int msgindex;

	s_assert(msg != NULL);
	if(msg == NULL)
		return;

	msgindex = cmd_hash(msg->cmd);

	for (ptr = msg_hash_table[msgindex]; ptr; ptr = ptr->next)
	{
		if(strcasecmp(msg->cmd, ptr->cmd) == 0)
		{
			ircd_free(ptr->cmd);
			if(last_ptr != NULL)
				last_ptr->next = ptr->next;
			else
				msg_hash_table[msgindex] = ptr->next;
			ircd_free(ptr);
			return;
		}
		last_ptr = ptr;
	}
}

/* hash_parse
 *
 * inputs	- command name
 * output	- pointer to struct Message
 * side effects - 
 */
static struct Message *
hash_parse(const char *cmd)
{
	struct MessageHash *ptr;
	int msgindex;
	
	msgindex = cmd_hash(cmd);

	for (ptr = msg_hash_table[msgindex]; ptr; ptr = ptr->next)
	{
		if(strcasecmp(cmd, ptr->cmd) == 0)
			return (ptr->msg);
	}

	return NULL;
}

/*
 * hash
 *
 * inputs	- char string
 * output	- hash index
 * side effects - NONE
 *
 * BUGS		- This was a horrible hash function, now its an okay one...
 */
static int
cmd_hash(const char *p)
{
	int hash_val = 0, q = 1, n;

	while (*p)
	{
		n = ToUpper(*p++);
		hash_val += ((n) + (q++ << 1)) ^ ((n) << 2);
	}
	/* note that 9 comes from 2^9 = MAX_MSG_HASH */
	return (hash_val >> 9) ^ (hash_val & (MAX_MSG_HASH-1));
}

/*
 * report_messages
 *
 * inputs	- pointer to client to report to
 * output	- NONE
 * side effects	- NONE
 */
void
report_messages(struct Client *source_p)
{
	int i;
	struct MessageHash *ptr;

	for (i = 0; i < MAX_MSG_HASH; i++)
	{
		for (ptr = msg_hash_table[i]; ptr; ptr = ptr->next)
		{
			s_assert(ptr->msg != NULL);
			s_assert(ptr->cmd != NULL);

			sendto_one_numeric(source_p, HOLD_QUEUE, RPL_STATSCOMMANDS, 
					   form_str(RPL_STATSCOMMANDS),
					   ptr->cmd, ptr->msg->count, 
					   ptr->msg->bytes, ptr->msg->rcount);
		}
	}
	send_pop_queue(source_p);
}

/* cancel_clients()
 *
 * inputs	- client who sent us the message, client with fake
 *		  direction, command
 * outputs	- a given warning about the fake direction
 * side effects -
 */
static void
cancel_clients(struct Client *client_p, struct Client *source_p, char *cmd)
{
	/* ok, fake prefix happens naturally during a burst on a nick
	 * collision with TS5, we cant kill them because one client has to
	 * survive, so we just send an error.
	 */
	if(IsServer(source_p) || IsMe(source_p))
	{
		sendto_realops_flags(UMODE_DEBUG, L_ALL,
				     "Message for %s[%s] from %s",
				     source_p->name, source_p->from->name,
				     get_server_name(client_p, SHOW_IP));
	}
	else
	{
		sendto_realops_flags(UMODE_DEBUG, L_ALL,
				     "Message for %s[%s@%s!%s] from %s (TS, ignored)",
				     source_p->name,
				     source_p->username,
				     source_p->host,
				     source_p->from->name,
				     get_server_name(client_p, SHOW_IP));
	}
}

/* remove_unknown()
 *
 * inputs	- client who gave us message, supposed sender, buffer
 * output	- 
 * side effects	- kills issued for clients, squits for servers
 */
static void
remove_unknown(struct Client *client_p, char *lsender, char *lbuffer)
{
	int slen = strlen(lsender);

	/* meepfoo	is a nickname (KILL)
	 * #XXXXXXXX	is a UID (KILL)
	 * #XX		is a SID (SQUIT)
	 * meep.foo	is a server (SQUIT)
	 */
	if((IsDigit(lsender[0]) && slen == 3) || 
	   (strchr(lsender, '.') != NULL))
	{
		sendto_realops_flags(UMODE_DEBUG, L_ALL,
				     "Unknown prefix (%s) from %s, Squitting %s",
				     lbuffer, get_server_name(client_p, SHOW_IP), lsender);

		sendto_one(client_p, POP_QUEUE,
			   ":%s SQUIT %s :(Unknown prefix (%s) from %s)",
			   get_id(&me, client_p), lsender, 
			   lbuffer, client_p->name);
	}
	else
		sendto_one(client_p, POP_QUEUE, ":%s KILL %s :%s (Unknown Client)", 
			   get_id(&me, client_p), lsender, me.name);
}



/*
 *
 *      parc    number of arguments ('sender' counted as one!)
 *      parv[0] pointer to 'sender' (may point to empty string) (not used)
 *      parv[1]..parv[parc-1]
 *              pointers to additional parameters, this is a NULL
 *              terminated list (parv[parc] == NULL).
 *
 * *WARNING*
 *      Numerics are mostly error reports. If there is something
 *      wrong with the message, just *DROP* it! Don't even think of
 *      sending back a neat error message -- big danger of creating
 *      a ping pong error message...
 */
static void
do_numeric(char numeric[], struct Client *client_p, struct Client *source_p, int parc, char *parv[])
{
	struct Client *target_p;
	struct Channel *chptr;

	if(parc < 2 || !IsServer(source_p))
		return;

	/* Remap low number numerics. */
	if(numeric[0] == '0')
		numeric[0] = '1';

	/*
	 * Prepare the parameter portion of the message into 'buffer'.
	 * (Because the buffer is twice as large as the message buffer
	 * for the socket, no overflow can occur here... ...on current
	 * assumptions--bets are off, if these are changed --msa)
	 * Note: if buffer is non-empty, it will begin with SPACE.
	 */
	if(parc > 1)
	{
		char *t = buffer;	/* Current position within the buffer */
		int i;
		int tl;		/* current length of presently being built string in t */
		for (i = 2; i < (parc - 1); i++)
		{
			tl = ircd_sprintf(t, " %s", parv[i]);
			t += tl;
		}
		ircd_sprintf(t, " :%s", parv[parc - 1]);
	}

	if((target_p = find_client(parv[1])) != NULL)
	{
		if(IsMe(target_p))
		{
			/*
			 * We shouldn't get numerics sent to us,
			 * any numerics we do get indicate a bug somewhere..
			 */
			/* ugh.  this is here because of nick collisions.  when two servers
			 * relink, they burst each other their nicks, then perform collides.
			 * if there is a nick collision, BOTH servers will kill their own
			 * nicks, and BOTH will kill the other servers nick, which wont exist,
			 * because it will have been already killed by the local server.
			 *
			 * unfortunately, as we cant guarantee other servers will do the
			 * "right thing" on a nick collision, we have to keep both kills.  
			 * ergo we need to ignore ERR_NOSUCHNICK. --fl_
			 */
			/* quick comment. This _was_ tried. i.e. assume the other servers
			 * will do the "right thing" and kill a nick that is colliding.
			 * unfortunately, it did not work. --Dianora
			 */
			/* note, now we send PING on server connect, we can
			 * also get ERR_NOSUCHSERVER..
			 */
			if(atoi(numeric) != ERR_NOSUCHNICK &&
			   atoi(numeric) != ERR_NOSUCHSERVER)
				sendto_realops_flags(UMODE_ALL, L_ADMIN,
						     "*** %s(via %s) sent a %s numeric to me: %s",
						     source_p->name,
						     client_p->name, numeric, buffer);
			return;
		}
		else if(target_p->from == client_p)
		{
			/* This message changed direction (nick collision?)
			 * ignore it.
			 */
			return;
		}

		/* csircd will send out unknown umode flag for +a (admin), drop it here. */
		if((atoi(numeric) == ERR_UMODEUNKNOWNFLAG) && MyClient(target_p))
			return;

		/* Fake it for server hiding, if its our client */
		sendto_one(target_p, POP_QUEUE, ":%s %s %s%s", 
			   get_id(source_p, target_p), numeric, 
			   get_id(target_p, target_p), buffer);
		return;
	}
	else if((chptr = find_channel(parv[1])) != NULL)
		sendto_channel_local(ALL_MEMBERS, chptr,
				     ":%s %s %s %s",
				     source_p->name, numeric, chptr->chname, buffer);
}


int
m_not_oper(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	sendto_one_numeric(source_p, POP_QUEUE, ERR_NOPRIVILEGES, form_str(ERR_NOPRIVILEGES));
	return 0;
}

int
m_unregistered(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	/* bit of a hack.
	 * I don't =really= want to waste a bit in a flag
	 * number_of_nick_changes is only really valid after the client
	 * is fully registered..
	 */
	if(client_p->localClient->number_of_nick_changes == 0)
	{
		sendto_one(client_p, POP_QUEUE, form_str(ERR_NOTREGISTERED), me.name);
		client_p->localClient->number_of_nick_changes++;
	}

	return 0;
}

int
m_registered(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	sendto_one(client_p, POP_QUEUE, form_str(ERR_ALREADYREGISTRED), me.name, source_p->name);
	return 0;
}

int
m_ignore(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
	return 0;
}
