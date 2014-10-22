/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Utillity functions used in the module.
 * @file mod.c
 *
 * @author Aaron Hurt <ahurt@anbcs.com>
 * @copyright 2013-2014 The FreeRADIUS Server Project.
 */

RCSID("$Id$");

#include <freeradius-devel/radiusd.h>

#include <libcouchbase/couchbase.h>
#include <json.h>

#include "mod.h"
#include "couchbase.h"
#include "jsonc_missing.h"

/** Delete a conneciton pool handle and free related resources
 *
 * Destroys the underlying Couchbase connection handle freeing any related
 * resources and closes the socket connection.
 *
 * @param  chandle The connection handle to destroy.
 * @return         Always returns 0 (success) in all conditions.
 */
static int _mod_conn_free(rlm_couchbase_handle_t *chandle)
{
	lcb_t cb_inst = chandle->handle;                /* couchbase instance */

	/* destroy/free couchbase instance */
	lcb_destroy(cb_inst);

	/* return */
	return 0;
}

/** Create a new connection pool handle
 *
 * Create a new connection to Couchbase within the pool and initialize
 * information associatd with the connection instance such as the cookie
 * payload and json tokener error value.
 *
 * @param  ctx      The connection parent context.
 * @param  instance The module instance.
 * @return          The new connection handle or NULL on error.
 */
void *mod_conn_create(TALLOC_CTX *ctx, void *instance)
{
	rlm_couchbase_t *inst = instance;           /* module instance pointer */
	rlm_couchbase_handle_t *chandle = NULL;     /* connection handle pointer */
	cookie_t *cookie = NULL;                    /* couchbase cookie */
	lcb_t cb_inst;                              /* couchbase connection instance */
	lcb_error_t cb_error = LCB_SUCCESS;         /* couchbase error status */

	/* create instance */
	cb_error = couchbase_init_connection(&cb_inst, inst->server, inst->bucket, inst->password);

	/* check couchbase instance */
	if (cb_error != LCB_SUCCESS) {
		ERROR("rlm_couchbase: failed to initiate couchbase connection: %s (0x%x)", lcb_strerror(NULL, cb_error), cb_error);
		/* destroy/free couchbase instance */
		lcb_destroy(cb_inst);
		/* fail */
		return NULL;
	}

	/* allocate memory for couchbase connection instance abstraction */
	chandle = talloc_zero(ctx, rlm_couchbase_handle_t);
	talloc_set_destructor(chandle, _mod_conn_free);

	cookie = talloc_zero(chandle, cookie_t);

	/* initialize cookie error holder */
	cookie->jerr = json_tokener_success;

	/* populate handle with allocated structs */
	chandle->cookie = cookie;
	chandle->handle = cb_inst;

	/* return handle struct */
	return chandle;
}

/** Check the health of a connection handle
 *
 * Attempt to determing the state of the Couchbase connection by requesting
 * a cluster statistics report.  Mark the connection as failed if the request
 * returns anything other than success.
 *
 * @param  instance The module instance (currently unused).
 * @param  handle   The connection handle.
 * @return          Returns 0 on success (alive) and -1 on error (unavailable).
 */
int mod_conn_alive(UNUSED void *instance, void *handle)
{
	rlm_couchbase_handle_t *chandle = handle;   /* connection handle pointer */
	lcb_t cb_inst = chandle->handle;            /* couchbase instance */
	lcb_error_t cb_error = LCB_SUCCESS;         /* couchbase error status */

	/* attempt to get server stats */
	if ((cb_error = couchbase_server_stats(cb_inst, NULL)) != LCB_SUCCESS) {
		/* log error */
		ERROR("rlm_couchbase: failed to get couchbase server stats: %s (0x%x)", lcb_strerror(NULL, cb_error), cb_error);
		/* error out */
		return -1;
	}
	return 0;
}

/** Build a JSON object map from the configuration "map" section
 *
 * Parse the "map" section from the module configuration file and store this
 * as a JSON object (key/value list) in the module instance.  This map will be
 * used to lookup and map attributes for all incoming accounting requests.
 *
 * @param  conf     Configuration section.
 * @param  instance The module instance.
 * @return          Returns 0 on success, -1 on error.
 */
int mod_build_attribute_element_map(CONF_SECTION *conf, void *instance)
{
	rlm_couchbase_t *inst = instance;   /* our module instance */
	CONF_SECTION *cs;                   /* module config section */
	CONF_ITEM *ci;                      /* config item */
	CONF_PAIR *cp;                      /* conig pair */
	const char *attribute, *element;    /* attribute and element names */

	/* find map section */
	cs = cf_section_sub_find(conf, "map");

	/* check section */
	if (!cs) {
		ERROR("rlm_couchbase: failed to find 'map' section in config");
		/* fail */
		return -1;
	}

	/* create attribute map object */
	inst->map = json_object_new_object();

	/* parse update section */
	for (ci = cf_item_find_next(cs, NULL); ci != NULL; ci = cf_item_find_next(cs, ci)) {
		/* validate item */
		if (!cf_item_is_pair(ci)) {
			ERROR("rlm_couchbase: failed to parse invalid item in 'map' section");
			/* free map */
			if (inst->map) {
				json_object_put(inst->map);
			}
			/* fail */
			return -1;
		}

		/* get value pair from item */
		cp = cf_itemtopair(ci);

		/* get pair name (element name) */
		element = cf_pair_attr(cp);

		/* get pair value (attribute name) */
		attribute = cf_pair_value(cp);

		/* add pair name and value */
		json_object_object_add(inst->map, attribute, json_object_new_string(element));

		/* debugging */
		DEBUG("rlm_couchbase: added attribute '%s' to element '%s' map to object", attribute, element);
	}

	/* debugging */
	DEBUG("rlm_couchbase: built attribute to element map %s", json_object_to_json_string(inst->map));

	/* return */
	return 0;
}

/** Map attributes to JSON element names
 *
 * Attempt to map the passed attribute name to the configured JSON element
 * name using the JSON object map mod_build_attribute_element_map().
 *
 * @param  name The character name of the requested attribute.
 * @param  map  The JSON object map to use for the lookup.
 * @param  buf  The buffer where the given element will be stored if found.
 * @return      Returns 0 on success, -1 on error.
 */
int mod_attribute_to_element(const char *name, json_object *map, void *buf)
{
	json_object *jval;  /* json object values */

	/* clear buffer */
	memset((char *) buf, 0, MAX_KEY_SIZE);

	/* attempt to map attribute */
	if (json_object_object_get_ex(map, name, &jval)) {
		/* copy and check size */
		if (strlcpy(buf, json_object_get_string(jval), MAX_KEY_SIZE) >= MAX_KEY_SIZE) {
			/* oops ... this value is bigger than our buffer ... error out */
			ERROR("rlm_couchbase: json map value larger than MAX_KEY_SIZE - %d", MAX_KEY_SIZE);
			/* return fail */
			return -1;
		}
		/* looks good */
		return 0;
	}

	/* debugging */
	DEBUG("rlm_couchbase: skipping attribute with no map entry - %s", name);

	/* default return */
	return -1;
}

/** Build value pairs from the passed JSON object and add to the request
 *
 * Parse the passed JSON object and create value pairs that will be injected into
 * the given request for authorization.
 *
 * Example JSON document structure:
 * @code{.json}
 * {
 *   "docType": "raduser",
 *   "userName": "test",
 *   "config": {
 *     "SHA-Password": {
 *       "value": "a94a8fe5ccb19ba61c4c0873d391e987982fbbd3",
 *       "op": ":="
 *     }
 *   },
 *   "reply": {
 *     "Reply-Message": {
 *       "value": "Hidey Ho!",
 *       "op": "="
 *     }
 *   }
 * }
 * @endcode
 *
 * @param  json    The JSON object representation of the user documnent.
 * @param  section The pair section ("config" or "reply").
 * @param  request The request to which the generated pairs should be added.
 */
void *mod_json_object_to_value_pairs(json_object *json, const char *section, REQUEST *request)
{
	json_object *jobj, *jval, *jop;     /* json object pointers */
	TALLOC_CTX *ctx;                    /* talloc context for pairmake */
	VALUE_PAIR *vp, **ptr;              /* value pair and value pair pointer for pairmake */

	/* assign ctx and vps for pairmake based on section */
	if (strcmp(section, "config") == 0) {
		ctx = request;
		ptr = &(request->config_items);
	} else if (strcmp(section, "reply") == 0) {
		ctx = request->reply;
		ptr = &(request->reply->vps);
	} else {
		/* log error - this shouldn't happen */
		RERROR("invalid section passed for pairmake");
		/* return */
		return NULL;
	}

	/* get config payload */
	if (json_object_object_get_ex(json, section, &jobj)) {
		/* make sure we have the correct type */
		if (!json_object_is_type(jobj, json_type_object)) {
			/* log error */
			RERROR("invalid json type for '%s' section - sections must be json objects", section);
			/* reuturn */
			return NULL;
		}
		/* loop through object */
		json_object_object_foreach(jobj, attribute, json_vp) {
			/* check for appropriate type in value and op */
			if (!json_object_is_type(json_vp, json_type_object)) {
				/* log error */
				RERROR("invalid json type for '%s' attribute - attributes must be json objects", attribute);
				/* return */
				return NULL;
			}
			/* debugging */
			RDEBUG("parsing '%s' attribute: %s => %s", section, attribute, json_object_to_json_string(json_vp));
			/* create pair from json object */
			if (json_object_object_get_ex(json_vp, "value", &jval) &&
				json_object_object_get_ex(json_vp, "op", &jop)) {
				/* make correct pairs based on json object type */
				switch (json_object_get_type(jval)) {
				case json_type_double:
				case json_type_int:
				case json_type_string:
					/* debugging */
					RDEBUG("adding '%s' attribute to '%s' section", attribute, section);
					/* add pair */
					vp = pairmake(ctx, ptr, attribute, json_object_get_string(jval),
						fr_str2int(fr_tokens, json_object_get_string(jop), 0));
					/* check pair */
					if (!vp) {
						RERROR("could not build value pair for '%s' attribute (%s)", attribute, fr_strerror());
						/* return */
						return NULL;
					}
					break;

				case json_type_object:
				case json_type_array:
					/* log error - we want to handle these eventually */
					RERROR("skipping unhandled nested json object or array value pair object");
					break;

				default:
					/* log error - this shouldn't ever happen */
					RERROR("skipping unhandled json type in value pair object");
					break;
				}
			} else {
				/* log error */
				RERROR("failed to get 'value' or 'op' element for '%s' attribute", attribute);
			}
		}
		/* return NULL */
		return NULL;
	}

	/* debugging */
	RDEBUG("couldn't find '%s' section in json object - not adding value pairs for this section", section);

	/* return NULL */
	return NULL;
}

/** Convert value pairs to json objects
 *
 * Take the passed value pair and convert it to a json-c JSON object.
 * This code is heavily based on the vp_prints_value_json() function
 * from src/lib/print.c.
 *
 * @param  request The request object.
 * @param  vp      The value pair to convert.
 * @return         Returns a JSON object.
 */
json_object *mod_value_pair_to_json_object(REQUEST *request, VALUE_PAIR *vp)
{
	char value[255];    /* radius attribute value */

	/* add this attribute/value pair to our json output */
	if (!vp->da->flags.has_tag) {
		unsigned int i;

		switch (vp->da->type) {
		case PW_TYPE_INTEGER:
			i = vp->vp_integer;
			goto print_int;

		case PW_TYPE_SHORT:
			i = vp->vp_short;
			goto print_int;

		case PW_TYPE_BYTE:
			i = vp->vp_byte;

		print_int:
			/* skip if we have flags */
			if (vp->da->flags.has_value) break;
#ifdef HAVE_JSON_OBJECT_NEW_INT64
			/* debug */
			RDEBUG3("creating new int64 for unsigned 32 bit int/byte/short '%s'", vp->da->name);
			/* return as 64 bit int - JSON spec does not support unsigned ints */
			return json_object_new_int64(i);
#else
			/* debug */
			RDEBUG3("creating new int for unsigned 32 bit int/byte/short '%s'", vp->da->name);
			/* return as 64 bit int - JSON spec does not support unsigned ints */
			return json_object_new_int(i);
#endif
		break;
		case PW_TYPE_SIGNED:
#ifdef HAVE_JSON_OBJECT_NEW_INT64
			/* debug */
			RDEBUG3("creating new int64 for signed 32 bit integer '%s'", vp->da->name);
			/* return as 64 bit int - json-c represents all ints as 64 bits internally */
			return json_object_new_int64(vp->vp_signed);
#else
			RDEBUG3("creating new int for signed 32 bit integer '%s'", vp->da->name);
			/* return as signed int */
			return json_object_new_int(vp->vp_signed);
#endif
		break;
		case PW_TYPE_INTEGER64:
#ifdef HAVE_JSON_OBJECT_NEW_INT64
			/* debug */
			RDEBUG3("creating new int64 for 64 bit integer '%s'", vp->da->name);
			/* return as 64 bit int - because it is a 64 bit int */
			return json_object_new_int64(vp->vp_integer64);
#else
			/* warning */
			RWARN("skipping 64 bit integer attribute '%s' - please upgrade json-c to 0.10+", vp->da->name);
#endif
		break;
		default:
			/* silence warnings - do nothing */
		break;
		}
	}

	/* keep going if not set above */
	switch (vp->da->type) {
	case PW_TYPE_STRING:
		/* debug */
		RDEBUG3("assigning string '%s' as string", vp->da->name);
		/* return string value */
		return json_object_new_string(vp->vp_strvalue);

	default:
		/* debug */
		RDEBUG3("assigning unhandled '%s' as string", vp->da->name);
		/* get standard value */
		vp_prints_value(value, sizeof(value), vp, 0);
		/* return string value from above */
		return json_object_new_string(value);
	}
}

/** Ensure accounting documents always contain a valid timestamp
 *
 * Inspect the given JSON object representation of an accounting document
 * fetched from Couchbase and ensuse it contains a valid (non NULL) timestamp value.
 *
 * @param  json JSON object representation of an accounting document.
 * @param  vps  The value pairs associated with the current accounting request.
 * @return      Returns 0 on success, -1 on error.
 */
int mod_ensure_start_timestamp(json_object *json, VALUE_PAIR *vps)
{
	json_object *jval;      /* json object value */
	struct tm tm;           /* struct to hold event time */
	time_t ts = 0;          /* values to hold time in seconds */
	VALUE_PAIR *vp;         /* values to hold value pairs */
	char value[255];        /* store radius attribute values and our timestamp */

	/* get our current start timestamp from our json body */
	if (json_object_object_get_ex(json, "startTimestamp", &jval) == 0) {
		/* debugging ... this shouldn't ever happen */
		DEBUG("rlm_couchbase: failed to find start timestamp in current json body");
		/* return */
		return -1;
	}

	/* check the value */
	if (strcmp(json_object_get_string(jval), "null") != 0) {
		/* debugging */
		DEBUG("rlm_couchbase: start timestamp looks good - nothing to do");
		/* already set - nothing else to do */
		return 0;
	}

	/* get current event timestamp */
	if ((vp = pairfind(vps, PW_EVENT_TIMESTAMP, 0, TAG_ANY)) != NULL) {
		/* get seconds value from attribute */
		ts = vp->vp_date;
	} else {
		/* debugging */
		DEBUG("rlm_couchbase: failed to find event timestamp in current request");
		/* return */
		return -1;
	}

	/* clear value */
	memset(value, 0, sizeof(value));

	/* get elapsed session time */
	if ((vp = pairfind(vps, PW_ACCT_SESSION_TIME, 0, TAG_ANY)) != NULL) {
		/* calculate diff */
		ts = (ts - vp->vp_integer);
		/* calculate start time */
		size_t length = strftime(value, sizeof(value), "%b %e %Y %H:%M:%S %Z", localtime_r(&ts, &tm));
		/* check length */
		if (length > 0) {
			/* debugging */
			DEBUG("rlm_couchbase: calculated start timestamp: %s", value);
			/* store new value in json body */
			json_object_object_add(json, "startTimestamp", json_object_new_string(value));
		} else {
			/* debugging */
			DEBUG("rlm_couchbase: failed to format calculated timestamp");
			/* return */
			return -1;
		}
	}

	/* default return */
	return 0;
}

/** Iterate over all client attribute pairs and create client pair data using JSON element names
 *
 * If we hit a CONF_SECTION we recurse and process its CONF_PAIRS as well to support nested
 * configurations sections.
 *
 * @param  client The new client config section using the mapped names.
 * @param  map    The client attribute section from the module configuration.
 * @param  json   JSON object representation of a client document fetched from Couchbase.
 * @return        Returns 0 on success, -1 on error.
 */
CC_HINT(nonnull) static int _mod_client_map_section(CONF_SECTION *client, CONF_SECTION const *map,
						    json_object *json, char const *docid)
{
	CONF_ITEM const *ci;

	for (ci = cf_item_find_next(map, NULL); ci != NULL; ci = cf_item_find_next(map, ci)) {
		CONF_PAIR const *cp;
		char const *attribute;
		char const *element;
		json_object *jval;

		/*
		 * Recursively process map subsection
		 */
		if (cf_item_is_section(ci)) {
			CONF_SECTION *cs, *cc;    /* local scoped for new section */

			cs = cf_itemtosection(ci);
			cc = cf_section_alloc(client, cf_section_name1(cs), cf_section_name2(cs));
			if (!cc) return -1;

			cf_section_add(client, cc);

			if (_mod_client_map_section(cc, cs, json, docid) != 0) {
				return -1;
			}
			/* continue on to the next item */
			continue;
		}

		/* create pair from item and get attribute name and value */
		cp = cf_itemtopair(ci);
		attribute = cf_pair_attr(cp);
		element = cf_pair_value(cp);

		/* attempt to find element in json object */
		if (!json_object_object_get_ex(json, element, &jval)) {
			/* skip this item */
			continue;
		}

		/* allocate config pair */
		cp = cf_pair_alloc(client, attribute, json_object_get_string(jval), T_OP_SET, T_SINGLE_QUOTED_STRING);

		/* check pair */
		if (!cp) {
			ERROR("rlm_couchbase: failed allocating config pair '%s' = '%s'", attribute, json_object_get_string(jval));
			return -1;
		}

		/* add pair to section */
		cf_item_add(client, cf_pairtoitem(cp));
	}

	/* return success */
	return 0;
}

/** Load client entries from Couchbase client documents on startup
 *
 * This function executes the view defined in the module configuration and loops
 * through all returned rows.  The view is called with "stale=false" to ensure the
 * most accurate data available when the view is called.  This will force an index
 * rebuild on this design document in Couchbase.  However, since this function is only
 * run once at sever startup this should not be a concern.
 *
 * @param  inst The module instance.
 * @param  cs   The client attribute configuration section.
 * @return      Returns 0 on success, -1 on error.
 */
int mod_load_client_documents(rlm_couchbase_t *inst, CONF_SECTION *cs)
{
	void *handle = NULL;                   /* connection pool handle */
	char vpath[256], docid[MAX_KEY_SIZE];  /* view path and document id */
	char error[512];                       /* view error return */
	int idx = 0;                           /* row array index counter */
	int retval = 0;                        /* return value */
	lcb_error_t cb_error = LCB_SUCCESS;    /* couchbase error holder */
	json_object *json, *jval;              /* json object holders */
	json_object *jrows = NULL;             /* json object to hold view rows */
	CONF_SECTION *client;                  /* freeradius config section */
	RADCLIENT *c;                          /* freeradius client */

	/* get handle */
	handle = fr_connection_get(inst->pool);

	/* check handle */
	if (!handle) return -1;

	/* set handle pointer */
	rlm_couchbase_handle_t *handle_t = handle;

	/* set couchbase instance */
	lcb_t cb_inst = handle_t->handle;

	/* set cookie */
	cookie_t *cookie = handle_t->cookie;

	/* check cookie */
	if (cookie) {
		/* clear cookie */
		memset(cookie, 0, sizeof(cookie_t));
	} else {
		/* log error */
		ERROR("rlm_couchbase: cookie not usable - possibly not allocated");
		/* set return */
		retval = -1;
		/* return */
		goto free_and_return;
	}

	/* build view path */
	snprintf(vpath, sizeof(vpath), "%s?stale=false", inst->client_view);

	/* init cookie error status */
	cookie->jerr = json_tokener_success;

	/* setup cookie tokener */
	cookie->jtok = json_tokener_new();

	/* query view for document */
	cb_error = couchbase_query_view(cb_inst, cookie, vpath, NULL);

	/* free json token */
	json_tokener_free(cookie->jtok);

	/* check error */
	if (cb_error != LCB_SUCCESS || cookie->jerr != json_tokener_success) {
		/* log error */
		ERROR("rlm_couchbase: failed to execute view request or parse return");
		/* set return */
		retval = -1;
		/* return */
		goto free_and_return;
	}

	/* debugging */
	DEBUG("rlm_couchbase: cookie->jobj == %s", json_object_to_json_string(cookie->jobj));

	/* check cookie */
	if (!cookie->jobj) {
		/* log error */
		ERROR("rlm_couchbase: failed to fetch view");
		/* set return */
		retval = -1;
		/* return */
		goto free_and_return;
	}

	/* check for error in json object */
	if (json_object_object_get_ex(cookie->jobj, "error", &json)) {
		/* build initial error buffer */
		strlcpy(error, json_object_get_string(json), sizeof(error));
		/* get error reason */
		if (json_object_object_get_ex(cookie->jobj, "reason", &json)) {
			/* append divider */
			strlcat(error, " - ", sizeof(error));
			/* append reason */
			strlcat(error, json_object_get_string(json), sizeof(error));
		}
		/* log error */
		ERROR("rlm_couchbase: view request failed with error: %s", error);
		/* set return */
		retval = -1;
		/* return */
		goto free_and_return;
	}

	/* check for document id in return */
	if (!json_object_object_get_ex(cookie->jobj, "rows", &json)) {
		/* log error */
		ERROR("rlm_couchbase: failed to fetch rows from view payload");
		/* set return */
		retval = -1;
		/* return */
		goto free_and_return;
	}

	/* get and hold rows */
	jrows = json_object_get(json);

	/* free cookie object */
	json_object_put(cookie->jobj);

	/* debugging */
	DEBUG("rlm_couchbase: jrows == %s", json_object_to_json_string(jrows));

	/* check for valid row value */
	if (!json_object_is_type(jrows, json_type_array) && json_object_array_length(jrows) < 1) {
		/* log error */
		ERROR("rlm_couchbase: couldn't find valid rows in view return");
		/* set return */
		retval = -1;
		/* return */
		goto free_and_return;
	}

	/* loop across all row elements */
	for (idx = 0; idx < json_object_array_length(jrows); idx++) {
		/* fetch current index */
		json = json_object_array_get_idx(jrows, idx);

		/* get document id */
		if (json_object_object_get_ex(json, "id", &jval)) {
			/* clear docid */
			memset(docid, 0, sizeof(docid));
			/* copy and check length */
			if (strlcpy(docid, json_object_get_string(jval), sizeof(docid)) >= sizeof(docid)) {
				ERROR("rlm_couchbase: document id from row longer than MAX_KEY_SIZE (%d)", MAX_KEY_SIZE);
				continue;
			}
		}

		/* check for valid doc id */
		if (docid[0] == 0) {
			WARN("rlm_couchbase: failed to fetch document id from row - skipping");
			continue;
		}

		/* debugging */
		DEBUG("rlm_couchbase: preparing to fetch docid '%s'", docid);

		/* reset  cookie error status */
		cookie->jerr = json_tokener_success;

		/* fetch document */
		cb_error = couchbase_get_key(cb_inst, cookie, docid);

		/* check error */
		if (cb_error != LCB_SUCCESS || cookie->jerr != json_tokener_success) {
			/* log error */
			ERROR("rlm_couchbase: failed to execute get request or parse return");
			/* set return */
			retval = -1;
			/* return */
			goto free_and_return;
		}

		/* debugging */
		DEBUG("rlm_couchbase: cookie->jobj == %s", json_object_to_json_string(cookie->jobj));

		/* allocate conf section */
		client = cf_section_alloc(NULL, "client", docid);

		if (_mod_client_map_section(client, cs, cookie->jobj, docid) != 0) {
			/* free config setion */
			talloc_free(client);
			/* set return */
			retval = -1;
			/* return */
			goto free_and_return;
		}

		/*
		 * @todo These should be parented from something.
		 */
		c = client_afrom_cs(NULL, client, false);
		if (!c) {
			ERROR("rlm_couchbase: failed to allocate client");
			/* free config setion */
			talloc_free(client);
			/* set return */
			retval = -1;
			/* return */
			goto free_and_return;
		}

		/*
		 * Client parents the CONF_SECTION which defined it.
		 */
		talloc_steal(c, client);

		/* attempt to add client */
		if (!client_add(NULL, c)) {
			ERROR("rlm_couchbase: failed to add client from %s, possible duplicate?", docid);
			/* free client */
			client_free(c);
			/* set return */
			retval = -1;
			/* return */
			goto free_and_return;
		}

		/* debugging */
		DEBUG("rlm_couchbase: client '%s' added", c->longname);

		/* free json object */
		json_object_put(cookie->jobj);
	}

	free_and_return:

	/* free json object */
	if (cookie->jobj) {
		json_object_put(cookie->jobj);
	}

	/* free rows */
	if (jrows) {
		json_object_put(jrows);
	}

	/* release handle */
	if (handle) {
		fr_connection_release(inst->pool, handle);
	}

	/* return */
	return retval;
}
