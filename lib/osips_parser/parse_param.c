/*
 * Generic Parameter Parser
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * -------
 * 2003-03-24 Created by janakj
 * 2003-04-07 shm duplication added (janakj)
 * 2003-04-07 URI class added (janakj)
 */

#include <string.h>
#include "str.h"
#include "ut.h"
#include "dprint.h"
#include "trim.h"
#include "mem.h"
#include "parse_param.h"


/*
 * Try to find out parameter name, recognized parameters
 * are q, expires and methods
 */
static inline void parse_contact_class(param_hooks_t* _h, param_t* _p)
{

	if (!_p->name.s) {
		LM_ERR("empty value\n");
		return;
	}
	switch(_p->name.s[0]) {
	case 'q':
	case 'Q':
		if (_p->name.len == 1) {
			_p->type = P_Q;
			_h->contact.q = _p;
		}
		break;

	case 'e':
	case 'E':
		if ((_p->name.len == 7) &&
		    (!strncasecmp(_p->name.s + 1, "xpires", 6))) {
			_p->type = P_EXPIRES;
			_h->contact.expires = _p;
		}
		break;

	case 'm':
	case 'M':
		if ((_p->name.len == 7) &&
		    (!strncasecmp(_p->name.s + 1, "ethods", 6))) {
			_p->type = P_METHODS;
			_h->contact.methods = _p;
		}
		break;

	case 'r':
	case 'R':
		if ((_p->name.len == 8) &&
		    (!strncasecmp(_p->name.s + 1, "eceived", 7))) {
			_p->type = P_RECEIVED;
			_h->contact.received = _p;
		}
		break;
	case '+':
		if ((_p->name.len == 13) &&
		    (!strncasecmp(_p->name.s + 1, "sip.instance", 12))) {
			_p->type = P_INSTANCE;
			_h->contact.instance = _p;
		}
		break;
	}
}


/*
 * Try to find out parameter name, recognized parameters
 * are transport, lr, r2, maddr
 */
static inline void parse_uri_class(param_hooks_t* _h, param_t* _p)
{

	if (!_p->name.s) {
		LM_ERR("empty value\n");
		return;
	}
	switch(_p->name.s[0]) {
	case 't':
	case 'T':
		if ((_p->name.len == 9) &&
		    (!strncasecmp(_p->name.s + 1, "ransport", 8))) {
			_p->type = P_TRANSPORT;
			_h->uri.transport = _p;
		} else if (_p->name.len == 2) {
			if (((_p->name.s[1] == 't') || (_p->name.s[1] == 'T')) &&
			    ((_p->name.s[2] == 'l') || (_p->name.s[2] == 'L'))) {
				_p->type = P_TTL;
				_h->uri.ttl = _p;
			}
		}
		break;

	case 'l':
	case 'L':
		if ((_p->name.len == 2) && ((_p->name.s[1] == 'r') || (_p->name.s[1] == 'R'))) {
			_p->type = P_LR;
			_h->uri.lr = _p;
		}
		break;

	case 'r':
	case 'R':
		if ((_p->name.len == 2) && (_p->name.s[1] == '2')) {
			_p->type = P_R2;
			_h->uri.r2 = _p;
		}
		break;

	case 'm':
	case 'M':
		if ((_p->name.len == 5) &&
		    (!strncasecmp(_p->name.s + 1, "addr", 4))) {
			_p->type = P_MADDR;
			_h->uri.maddr = _p;
		}
		break;

	case 'd':
	case 'D':
		if ((_p->name.len == 5) &&
		    (!strncasecmp(_p->name.s + 1, "stip", 4))) {
			_p->type = P_DSTIP;
			_h->uri.dstip = _p;
		} else if ((_p->name.len == 7) &&
			   (!strncasecmp(_p->name.s + 1, "stport", 6))) {
			_p->type = P_DSTPORT;
			_h->uri.dstport = _p;
		}
		break;
	}

}


/*
 * Parse quoted string in a parameter body
 * return the string without quotes in _r
 * parameter and update _s to point behind the
 * closing quote
 */
static inline int parse_quoted_param(str* _s, str* _r)
{
	char* end_quote;

	     /* The string must have at least
	      * surrounding quotes
	      */
	if (_s->len < 2) {
		return -1;
	}

	     /* Skip opening quote */
	_s->s++;
	_s->len--;


	     /* Find closing quote */
	end_quote = q_memchr(_s->s, '\"', _s->len);

	     /* Not found, return error */
	if (!end_quote) {
		return -2;
	}

	     /* Let _r point to the string without
	      * surrounding quotes
	      */
	_r->s = _s->s;
	_r->len = end_quote - _s->s;

	     /* Update _s parameter to point
	      * behind the closing quote
	      */
	_s->len -= (end_quote - _s->s + 1);
	_s->s = end_quote + 1;

	     /* Everything went OK */
	return 0;
}


/*
 * Parse unquoted token in a parameter body
 * let _r point to the token and update _s
 * to point right behind the token
 */
static inline int parse_token_param(str* _s, str* _r)
{
	int i;

	     /* There is nothing to parse,
	      * return error
	      */
	if (_s->len == 0) {
		return -1;
	}

	     /* Save the beginning of the
	      * token in _r->s
	      */
	_r->s = _s->s;

	     /* Iterate through the
	      * token body
	      */
	for(i = 0; i < _s->len; i++) {

		     /* All these characters
		      * mark end of the token
		      */
		switch(_s->s[i]) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
		case ',':
		case ';':
			     /* So if you find
			      * any of them
			      * stop iterating
			      */
			goto out;
		}
	}
 out:
	if (i == 0) {
		return -1;
        }

	     /* Save length of the token */
        _r->len = i;

	     /* Update _s parameter so it points
	      * right behind the end of the token
	      */
	_s->s = _s->s + i;
	_s->len -= i;

	     /* Everything went OK */
	return 0;
}


/*
 * Parse a parameter name
 */
static inline void parse_param_name(str* _s, pclass_t _c, param_hooks_t* _h, param_t* _p)
{

	if (!_s->s) {
		LM_DBG("empty parameter\n");
		return;
	}

	_p->name.s = _s->s;

	while(_s->len) {
		switch(_s->s[0]) {
		case ' ':
		case '\t':
		case '\r':
		case '\n':
		case ';':
		case ',':
		case '=':
			goto out;
		}
		_s->s++;
		_s->len--;
	}

 out:
	_p->name.len = _s->s - _p->name.s;

	switch(_c) {
	case CLASS_CONTACT: parse_contact_class(_h, _p); break;
	case CLASS_URI:     parse_uri_class(_h, _p);     break;
	default: break;
	}
}





/*
 * Parse body of a parameter. It can be quoted string or
 * a single token.
 */
static inline int parse_param_body(str* _s, param_t* _c)
{
	if (_s->s[0] == '\"') {
		if (parse_quoted_param(_s, &(_c->body)) < 0) {
			LM_ERR("failed to parse quoted string\n");
			return -2;
		}
	} else {
		if (parse_token_param(_s, &(_c->body)) < 0) {
			LM_ERR("failed to parse token\n");
			return -3;
		}
	}

	return 0;
}


/*
 * Parse parameters
 * _s is string containing parameters, it will be updated to point behind the parameters
 * _c is class of parameters
 * _h is pointer to structure that will be filled with pointer to well known parameters
 * linked list of parsed parameters will be stored in
 * the variable _p is pointing to
 * The function returns 0 on success and negative number
 * on an error
 */
int parse_params(str* _s, pclass_t _c, param_hooks_t* _h, param_t** _p)
{
	param_t* t;
	param_t* last;

	if (!_s || !_h || !_p) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	memset(_h, 0, sizeof(param_hooks_t));
	last = NULL;
	*_p = 0;

	if (!_s->s) { /* no parameters at all -- we're done */
		LM_DBG("empty uri params, skipping\n");
		return 0;
	}

	LM_DBG("Parsing params for:[%.*s]\n",_s->len,_s->s);

	while(1) {
		t = (param_t*)pkg_malloc(sizeof(param_t));
		if (t == 0) {
			LM_ERR("no pkg memory left\n");
			goto error;
		}
		memset(t, 0, sizeof(param_t));

		parse_param_name(_s, _c, _h, t);
		trim_leading(_s);

		if (_s->len == 0) { /* The last parameter without body */
			t->len = t->name.len;
			goto ok;
		}

		if (_s->s[0] == '=') {
			_s->s++;
			_s->len--;
			trim_leading(_s);

			if (_s->len == 0) {
				LM_ERR("body missing\n");
				goto error;
			}

			if (parse_param_body(_s, t) < 0) {
				LM_ERR("failed to parse param body\n");
				goto error;
			}

			t->len = _s->s - t->name.s;

			trim_leading(_s);
			if (_s->len == 0) {
				goto ok;
			}
		} else {
			t->len = t->name.len;
		}

		if (_s->s[0]==',') goto ok; /* To be able to parse header parameters */
		if (_s->s[0]=='>') goto ok; /* To be able to parse URI parameters */

		if (_s->s[0] != ';') {
			LM_ERR("invalid character, ; expected, found %c \n",_s->s[0]);
			goto error;
		}

		_s->s++;
		_s->len--;
		trim_leading(_s);

		if (_s->len == 0) {
			LM_ERR("param name missing after ;\n");
			goto error;
		}

		if (last) {last->next=t;} else {*_p = t;}
		last = t;
	}

error:
	if (t) pkg_free(t);
	free_params(*_p);
	*_p = 0;
	return -2;

ok:
	if (last) {last->next=t;} else {*_p = t;}
	_h->last_param = last = t;
	return 0;
}


/*
 * Free linked list of parameters
 */
static inline void do_free_params(param_t* _p, int _shm)
{
	param_t* ptr;

	while(_p) {
		ptr = _p;
		_p = _p->next;
		if (_shm) shm_free(ptr);
		else pkg_free(ptr);
	}
}


/*
 * Free linked list of parameters
 */
void free_params(param_t* _p)
{
	do_free_params(_p, 0);
}


/*
 * Free linked list of parameters
 */
void shm_free_params(param_t* _p)
{
	do_free_params(_p, 1);
}


/*
 * Print a parameter structure, just for debugging
 */
static inline void print_param(param_t* _p)
{
	char* type;

	LM_DBG("---param(%p)---\n", _p);

	switch(_p->type) {
	case P_OTHER:     type = "P_OTHER";     break;
	case P_Q:         type = "P_Q";         break;
	case P_EXPIRES:   type = "P_EXPIRES";   break;
	case P_METHODS:   type = "P_METHODS";   break;
	case P_TRANSPORT: type = "P_TRANSPORT"; break;
	case P_LR:        type = "P_LR";        break;
	case P_R2:        type = "P_R2";        break;
	case P_MADDR:     type = "P_MADDR";     break;
	case P_TTL:       type = "P_TTL";       break;
	case P_RECEIVED:  type = "P_RECEIVED";  break;
	case P_DSTIP:     type = "P_DSTIP";     break;
	case P_DSTPORT:   type = "P_DSTPORT";   break;
	default:          type = "UNKNOWN";     break;
	}

	LM_DBG("type: %s\n", type);
	LM_DBG("name: \'%.*s\'\n", _p->name.len, _p->name.s);
	LM_DBG("body: \'%.*s\'\n", _p->body.len, _p->body.s);
	LM_DBG("len : %d\n", _p->len);
	LM_DBG("---/param---\n");
}


/*
 * Print linked list of parameters, just for debugging
 */
void print_params(param_t* _p)
{
	param_t* ptr;

	ptr = _p;
	while(ptr) {
		print_param(ptr);
		ptr = ptr->next;
	}
}


/*
 * Duplicate linked list of parameters
 */
static inline int do_duplicate_params(param_t** _n, param_t* _p, int _shm)
{
	param_t* last, *ptr, *t;

	if (!_n) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	last = 0;
	*_n = 0;
	ptr = _p;
	while(ptr) {
		if (_shm) {
			t = (param_t*)shm_malloc(sizeof(param_t));
		} else {
			t = (param_t*)pkg_malloc(sizeof(param_t));
		}
		if (!t) {
			LM_ERR("no more pkg memory\n");
			goto err;
		}
		memcpy(t, ptr, sizeof(param_t));
		t->next = 0;

		if (!*_n) *_n = t;
		if (last) last->next = t;
		last = t;

		ptr = ptr->next;
	}
	return 0;

 err:
	do_free_params(*_n, _shm);
	return -2;
}


/*
 * Duplicate linked list of parameters
 */
int duplicate_params(param_t** _n, param_t* _p)
{
	return do_duplicate_params(_n, _p, 0);
}


/*
 * Duplicate linked list of parameters
 */
int shm_duplicate_params(param_t** _n, param_t* _p)
{
	return do_duplicate_params(_n, _p, 1);
}
