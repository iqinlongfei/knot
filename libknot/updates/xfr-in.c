/*  Copyright (C) 2011 CZ.NIC Labs

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <urcu.h>

#include "updates/xfr-in.h"

#include "nameserver/name-server.h"
#include "util/debug.h"
// #include "knot/zone/zone-dump.h"
// #include "knot/zone/zone-load.h"
#include "packet/packet.h"
#include "dname.h"
#include "zone/zone.h"
#include "packet/query.h"
#include "packet/response.h"
#include "util/error.h"
#include "updates/changesets.h"

/*----------------------------------------------------------------------------*/
/* Non-API functions                                                          */
/*----------------------------------------------------------------------------*/

static int xfrin_create_query(const knot_zone_contents_t *zone, uint16_t qtype,
                              uint16_t qclass, uint8_t *buffer, size_t *size)
{
	knot_packet_t *pkt = knot_packet_new(KNOT_PACKET_PREALLOC_QUERY);
	CHECK_ALLOC_LOG(pkt, KNOT_ENOMEM);

	/*! \todo Get rid of the numeric constant. */
	int rc = knot_packet_set_max_size(pkt, 512);
	if (rc != KNOT_EOK) {
		knot_packet_free(&pkt);
		return KNOT_ERROR;
	}

	rc = knot_query_init(pkt);
	if (rc != KNOT_EOK) {
		knot_packet_free(&pkt);
		return KNOT_ERROR;
	}

	knot_question_t question;

	const knot_node_t *apex = knot_zone_contents_apex(zone);
	knot_dname_t *qname = knot_node_get_owner(apex);

	/* Retain qname until the question is freed. */
	knot_dname_retain(qname);

	// this is ugly!!
	question.qname = (knot_dname_t *)qname;
	question.qtype = qtype;
	question.qclass = qclass;

	rc = knot_query_set_question(pkt, &question);
	if (rc != KNOT_EOK) {
		knot_dname_release(question.qname);
		knot_packet_free(&pkt);
		return KNOT_ERROR;
	}

	/* Add SOA RR to authority section for IXFR. */
	if (qtype == KNOT_RRTYPE_IXFR) {
		/*!
		 *  \todo Implement properly.
		 */
		const knot_rrset_t *soa = knot_node_rrset(apex, KNOT_RRTYPE_SOA);
		knot_query_add_rrset_authority(pkt, soa);
	}

	/* Set random query ID. */
	knot_packet_set_random_id(pkt);

	/*! \todo OPT RR ?? */

	uint8_t *wire = NULL;
	size_t wire_size = 0;
	rc = knot_packet_to_wire(pkt, &wire, &wire_size);
	if (rc != KNOT_EOK) {
		debug_knot_xfr("Failed to write packet to wire.\n");
		knot_dname_release(question.qname);
		knot_packet_free(&pkt);
		return KNOT_ERROR;
	}

	if (wire_size > *size) {
		debug_knot_xfr("Not enough space provided for the wire "
		               "format of the query.\n");
		knot_packet_free(&pkt);
		return KNOT_ESPACE;
	}

	memcpy(buffer, wire, wire_size);
	*size = wire_size;

	debug_knot_xfr("Created query of size %zu.\n", *size);
	knot_packet_dump(pkt);

	knot_packet_free(&pkt);

	/* Release qname. */
	knot_dname_release(question.qname);

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static uint32_t xfrin_serial_difference(uint32_t local, uint32_t remote)
{
	return (((int64_t)remote - local) % ((int64_t)1 << 32));
}

/*----------------------------------------------------------------------------*/
/* API functions                                                              */
/*----------------------------------------------------------------------------*/

int xfrin_create_soa_query(const knot_zone_contents_t *zone, uint8_t *buffer,
                           size_t *size)
{
	return xfrin_create_query(zone, KNOT_RRTYPE_SOA,
	                          KNOT_CLASS_IN, buffer, size);
}

/*----------------------------------------------------------------------------*/

int xfrin_transfer_needed(const knot_zone_contents_t *zone,
                          knot_packet_t *soa_response)
{
	// first, parse the rest of the packet
	assert(!knot_packet_is_query(soa_response));
	debug_knot_xfr("Response - parsed: %zu, total wire size: %zu\n",
	         soa_response->parsed, soa_response->size);
	int ret;

	if (soa_response->parsed < soa_response->size) {
		ret = knot_packet_parse_rest(soa_response);
		if (ret != KNOT_EOK) {
			return KNOT_EMALF;
		}
	}

	/*
	 * Retrieve the local Serial
	 */
	const knot_rrset_t *soa_rrset =
		knot_node_rrset(knot_zone_contents_apex(zone),
		                KNOT_RRTYPE_SOA);
	if (soa_rrset == NULL) {
		char *name = knot_dname_to_str(knot_node_owner(
				knot_zone_contents_apex(zone)));
		debug_knot_xfr("SOA RRSet missing in the zone %s!\n", name);
		free(name);
		return KNOT_ERROR;
	}

	int64_t local_serial = knot_rdata_soa_serial(
		knot_rrset_rdata(soa_rrset));
	if (local_serial < 0) {
		char *name = knot_dname_to_str(knot_rrset_owner(soa_rrset));
		debug_knot_xfr("Malformed data in SOA of zone %s\n", name);
		free(name);
		return KNOT_EMALF;	// maybe some other error
	}

	/*
	 * Retrieve the remote Serial
	 */
	// the SOA should be the first (and only) RRSet in the response
	soa_rrset = knot_packet_answer_rrset(soa_response, 0);
	if (soa_rrset == NULL
	    || knot_rrset_type(soa_rrset) != KNOT_RRTYPE_SOA) {
		return KNOT_EMALF;
	}

	int64_t remote_serial = knot_rdata_soa_serial(
		knot_rrset_rdata(soa_rrset));
	if (remote_serial < 0) {
		return KNOT_EMALF;	// maybe some other error
	}

	uint32_t diff = xfrin_serial_difference(local_serial, remote_serial);
	return (diff >= 1 && diff <= (((uint32_t)1 << 31) - 1)) ? 1 : 0;
}

/*----------------------------------------------------------------------------*/

int xfrin_create_axfr_query(const knot_zone_contents_t *zone, uint8_t *buffer,
                            size_t *size)
{
	return xfrin_create_query(zone, KNOT_RRTYPE_AXFR,
	                          KNOT_CLASS_IN, buffer, size);
}

/*----------------------------------------------------------------------------*/

int xfrin_create_ixfr_query(const knot_zone_contents_t *zone, uint8_t *buffer,
                            size_t *size)
{
	return xfrin_create_query(zone, KNOT_RRTYPE_IXFR,
	                          KNOT_CLASS_IN, buffer, size);
}

/*----------------------------------------------------------------------------*/

int xfrin_process_axfr_packet(const uint8_t *pkt, size_t size,
                              knot_zone_contents_t **zone)
{
	if (pkt == NULL || zone == NULL) {
		debug_knot_xfr("Wrong parameters supported.\n");
		return KNOT_EBADARG;
	}

	knot_packet_t *packet =
			knot_packet_new(KNOT_PACKET_PREALLOC_NONE);
	if (packet == NULL) {
		debug_knot_xfr("Could not create packet structure.\n");
		return KNOT_ENOMEM;
	}

	int ret = knot_packet_parse_from_wire(packet, pkt, size, 1);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Could not parse packet: %s.\n",
		               knot_strerror(ret));
		knot_packet_free(&packet);
		/*! \todo Cleanup. */
		return KNOT_EMALF;
	}

	knot_rrset_t *rr = NULL;
	ret = knot_packet_parse_next_rr_answer(packet, &rr);

	if (ret != KNOT_EOK) {
		debug_knot_xfr("Could not parse first Answer RR: %s.\n",
		               knot_strerror(ret));
		knot_packet_free(&packet);
		/*! \todo Cleanup. */
		return KNOT_EMALF;
	}

	if (rr == NULL) {
		debug_knot_xfr("No RRs in the packet.\n");
		knot_packet_free(&packet);
		/*! \todo Cleanup. */
		return KNOT_EMALF;
	}

	/*! \todo We should probably test whether the Question of the first
	 *        message corresponds to the SOA RR.
	 */

	knot_node_t *node = NULL;
	int in_zone = 0;

	if (*zone == NULL) {
		// create new zone
		/*! \todo Ensure that the packet is the first one. */
		if (knot_rrset_type(rr) != KNOT_RRTYPE_SOA) {
			debug_knot_xfr("No zone created, but the first RR in "
			               "Answer is not a SOA RR.\n");
			knot_packet_free(&packet);
			knot_node_free(&node, 0, 0);
			knot_rrset_deep_free(&rr, 1, 1, 1);
			/*! \todo Cleanup. */
			return KNOT_EMALF;
		}

		if (knot_dname_compare(knot_rrset_owner(rr),
		                       knot_packet_qname(packet)) != 0) {
DEBUG_KNOT_XFR(
			char *rr_owner =
				knot_dname_to_str(knot_rrset_owner(rr));
			char *qname = knot_dname_to_str(
				knot_packet_qname(packet));

			debug_knot_xfr("Owner of the first SOA RR (%s) does not"
			          " match QNAME (%s).\n", rr_owner, qname);

			free(rr_owner);
			free(qname);
);
			/*! \todo Cleanup. */
			knot_packet_free(&packet);
			knot_node_free(&node, 0, 0);
			knot_rrset_deep_free(&rr, 1, 1, 1);
			return KNOT_EMALF;
		}

		node = knot_node_new(rr->owner, NULL, 0);
		if (node == NULL) {
			debug_knot_xfr("Failed to create new node.\n");
			knot_packet_free(&packet);
			knot_rrset_deep_free(&rr, 1, 1, 1);
			return KNOT_ENOMEM;
		}

		// the first RR is SOA and its owner and QNAME are the same
		// create the zone
		/*! \todo Set the zone pointer to the contents. */
		*zone = knot_zone_contents_new(node, 0, 1, NULL);
		if (*zone == NULL) {
			debug_knot_xfr("Failed to create new zone.\n");
			knot_packet_free(&packet);
			knot_node_free(&node, 0, 0);
			knot_rrset_deep_free(&rr, 1, 1, 1);
			/*! \todo Cleanup. */
			return KNOT_ENOMEM;
		}

		in_zone = 1;
		assert(node->owner == rr->owner);
		// add the RRSet to the node
		//ret = knot_node_add_rrset(node, rr, 0);
		ret = knot_zone_contents_add_rrset(*zone, rr, &node,
		                                    KNOT_RRSET_DUPL_MERGE, 1);
		if (ret < 0) {
			debug_knot_xfr("Failed to add RRSet to zone node: %s.\n",
			          knot_strerror(ret));
			knot_packet_free(&packet);
			knot_node_free(&node, 0, 0);
			knot_rrset_deep_free(&rr, 1, 1, 1);
			/*! \todo Cleanup. */
			return KNOT_ERROR;
		} else if (ret > 0) {
			// merged, free the RRSet
			knot_rrset_deep_free(&rr, 1, 0, 0);
		}

		// take next RR
		ret = knot_packet_parse_next_rr_answer(packet, &rr);
	}

	while (ret == KNOT_EOK && rr != NULL) {
		// process the parsed RR

		debug_knot_xfr("\nNext RR:\n\n");
		knot_rrset_dump(rr, 0);

		if (node != NULL
		    && knot_dname_compare(rr->owner, node->owner) != 0) {
			if (!in_zone) {
				// this should not happen
				assert(0);
				// the node is not in the zone and the RR has
				// other owner, so a new node must be created
				// insert the old node to the zone
			}

			node = NULL;
		}

		if (knot_rrset_type(rr) == KNOT_RRTYPE_SOA) {
			// this must be the last SOA, do not do anything more
			// discard the RR
			assert(knot_zone_contents_apex((*zone)) != NULL);
			assert(knot_node_rrset(knot_zone_contents_apex((*zone)),
			                       KNOT_RRTYPE_SOA) != NULL);
			debug_knot_xfr("Found last SOA, transfer finished.\n");
			knot_rrset_deep_free(&rr, 1, 1, 1);
			knot_packet_free(&packet);
			return 1;
		}

		if (knot_rrset_type(rr) == KNOT_RRTYPE_RRSIG) {
			// RRSIGs require special handling, as there are no
			// nodes for them
			knot_rrset_t *tmp_rrset = NULL;
			ret = knot_zone_contents_add_rrsigs(*zone, rr,
			         &tmp_rrset, &node, KNOT_RRSET_DUPL_MERGE, 1);
			if (ret < 0) {
				debug_knot_xfr("Failed to add RRSIGs.\n");
				knot_packet_free(&packet);
				knot_node_free(&node, 1, 0); // ???
				knot_rrset_deep_free(&rr, 1, 1, 1);
				return KNOT_ERROR;  /*! \todo Other error code. */
			} else if (ret == 1) {
				knot_rrset_deep_free(&rr, 1, 0, 0);
			} else if (ret == 2) {
				// should not happen
				assert(0);
//				knot_rrset_deep_free(&rr, 1, 1, 1);
			} else {
				assert(tmp_rrset->rrsigs == rr);
			}

			// parse next RR
			ret = knot_packet_parse_next_rr_answer(packet, &rr);

			continue;
		}

		knot_node_t *(*get_node)(const knot_zone_contents_t *,
		                         const knot_dname_t *) = NULL;
		int (*add_node)(knot_zone_contents_t *, knot_node_t *, int,
		                uint8_t, int) = NULL;

		if (knot_rrset_type(rr) == KNOT_RRTYPE_NSEC3) {
			get_node = knot_zone_contents_get_nsec3_node;
			add_node = knot_zone_contents_add_nsec3_node;
		} else {
			get_node = knot_zone_contents_get_node;
			add_node = knot_zone_contents_add_node;
		}

		if (node == NULL && (node = get_node(*zone,
		                               knot_rrset_owner(rr))) != NULL) {
			// the node for this RR was found in the zone
			debug_knot_xfr("Found node for the record in zone.\n");
			in_zone = 1;
		}

		if (node == NULL) {
			// a new node for the RR is required but it is not
			// in the zone
			node = knot_node_new(rr->owner, NULL, 0);
			if (node == NULL) {
				debug_knot_xfr("Failed to create new node.\n");
				knot_packet_free(&packet);
				knot_rrset_deep_free(&rr, 1, 1, 1);
				return KNOT_ENOMEM;
			}
			debug_knot_xfr("Created new node for the record.\n");

			// insert the node into the zone
			ret = knot_node_add_rrset(node, rr, 1);
			if (ret < 0) {
				debug_knot_xfr("Failed to add RRSet to node\n");
				knot_packet_free(&packet);
				knot_node_free(&node, 1, 0); // ???
				knot_rrset_deep_free(&rr, 1, 1, 1);
				return KNOT_ERROR;
			} else if (ret > 0) {
				// should not happen, this is new node
				assert(0);
//				knot_rrset_deep_free(&rr, 1, 0, 0);
			}

			ret = add_node(*zone, node, 1, 0, 1);
			if (ret != KNOT_EOK) {
				debug_knot_xfr("Failed to add node to zone.\n");
				knot_packet_free(&packet);
				knot_node_free(&node, 1, 0); // ???
				knot_rrset_deep_free(&rr, 1, 1, 1);
				return KNOT_ERROR;
			}

			in_zone = 1;
		} else {
			assert(in_zone);

			ret = knot_zone_contents_add_rrset(*zone, rr, &node,
			                            KNOT_RRSET_DUPL_MERGE, 1);
			if (ret < 0) {
				debug_knot_xfr("Failed to add RRSet to zone:"
				               "%s.\n", knot_strerror(ret));
				return KNOT_ERROR;
			} else if (ret > 0) {
				// merged, free the RRSet
				knot_rrset_deep_free(&rr, 1, 0, 0);
			}

		}

		rr = NULL;

		// parse next RR
		ret = knot_packet_parse_next_rr_answer(packet, &rr);
	}

	assert(ret != KNOT_EOK || rr == NULL);

	if (ret < 0) {
		// some error in parsing
		debug_knot_xfr("Could not parse next RR: %s.\n",
		               knot_strerror(ret));
		knot_packet_free(&packet);
		knot_node_free(&node, 0, 0);
		knot_rrset_deep_free(&rr, 1, 1, 1);
		/*! \todo Cleanup. */
		return KNOT_EMALF;
	}

	assert(ret == KNOT_EOK);
	assert(rr == NULL);

	// if the last node is not yet in the zone, insert
	if (!in_zone) {
		assert(node != NULL);
		ret = knot_zone_contents_add_node(*zone, node, 1, 0, 1);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add last node into zone.\n");
			knot_packet_free(&packet);
			knot_node_free(&node, 1, 0);
			return KNOT_ERROR;	/*! \todo Other error */
		}
	}

	knot_packet_free(&packet);
	debug_knot_xfr("Processed one AXFR packet successfully.\n");

	return (ret == KNOT_EOK) ? KNOT_EOK : KNOT_EMALF;
}

/*----------------------------------------------------------------------------*/

static int xfrin_parse_first_rr(knot_packet_t **packet, const uint8_t *pkt,
                                size_t size, knot_rrset_t **rr)
{
	*packet = knot_packet_new(KNOT_PACKET_PREALLOC_NONE);
	if (packet == NULL) {
		debug_knot_xfr("Could not create packet structure.\n");
		return KNOT_ENOMEM;
	}

	int ret = knot_packet_parse_from_wire(*packet, pkt, size, 1);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Could not parse packet: %s.\n",
		               knot_strerror(ret));
		knot_packet_free(packet);
		return KNOT_EMALF;
	}

	ret = knot_packet_parse_next_rr_answer(*packet, rr);

	if (ret != KNOT_EOK) {
		debug_knot_xfr("Could not parse first Answer RR: %s.\n",
		          knot_strerror(ret));
		knot_packet_free(packet);
		return KNOT_EMALF;
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

int xfrin_process_ixfr_packet(const uint8_t *pkt, size_t size,
                              knot_changesets_t **chs)
{
	if (pkt == NULL || chs == NULL) {
		debug_knot_xfr("Wrong parameters supported.\n");
		return KNOT_EBADARG;
	}

	knot_packet_t *packet = NULL;
//	knot_rrset_t *soa1 = NULL;
//	knot_rrset_t *soa2 = NULL;
	knot_rrset_t *rr = NULL;

	int ret;
	
	if ((ret = xfrin_parse_first_rr(&packet, pkt, size, &rr))
	     != KNOT_EOK) {
		return ret;
	}

	assert(packet != NULL);
	
	// state of the transfer
	// -1 .. a SOA is expected to create a new changeset
	int state;

	if (rr == NULL) {
		debug_knot_xfr("No RRs in the packet.\n");
		knot_packet_free(&packet);
		/*! \todo Some other action??? */
		return KNOT_EMALF;
	}
	
	if (*chs == NULL) {
		ret = knot_changeset_allocate(chs);
		if (ret != KNOT_EOK) {
			knot_rrset_deep_free(&rr, 1, 1, 1);
			knot_packet_free(&packet);
			return ret;
		}
		
		// the first RR must be a SOA
		if (knot_rrset_type(rr) != KNOT_RRTYPE_SOA) {
			debug_knot_xfr("First RR is not a SOA RR!\n");
			knot_rrset_deep_free(&rr, 1, 1, 1);
			ret = KNOT_EMALF;
			goto cleanup;
		}
		
		// just store the first SOA for later use
		(*chs)->first_soa = rr;
		state = -1;
		
		// parse the next one
		ret = knot_packet_parse_next_rr_answer(packet, &rr);
	} else {
		if ((*chs)->first_soa == NULL) {
			debug_knot_xfr("Changesets don't contain frist SOA!\n");
			return KNOT_EBADARG;
		}
	}

	/*
	 * Process the next RR. Different requirements are in place in 
	 * different cases:
	 *
	 * 1) Last changeset has both soa_from and soa_to.
	 *    a) The next RR is a SOA.
	 *      i) The next RR is equal to the first_soa saved in changesets.
	 *         This denotes the end of the transfer. It may be dropped and
	 *         the end should be signalised by returning positive value.
	 *
	 *      ii) The next RR is some other SOA.
	 *          This means a start of new changeset - create it and add it 
	 *          to the list.
	 *
	 *    b) The next RR is not a SOA.
	 *       Put the RR into the ADD part of the last changeset as this is
	 *       not finished yet. Continue while SOA is not encountered. Then
	 *       jump to 1-a.
	 * 
	 * 2) Last changeset has only the soa_from and does not have soa_to.
	 *    a) The next RR is a SOA.
	 *       This means start of the ADD section. Put the SOA to the 
	 *       changeset. Continue adding RRs to the ADD section while SOA
	 *       is not encountered. This is identical to 1-b.
	 *
	 *    b) The next RR is not a SOA.
	 *       This means the REMOVE part is not finished yet. Add the RR to
	 *       the REMOVE part. Continue adding next RRs until a SOA is 
	 *       encountered. Then jump to 2-a.
	 */
	
	// first, find out in what state we are
	/*! \todo It would be more elegant to store the state in the 
	 *        changesets structure, or in some place persistent between
	 *        calls to this function.
	 */
	if (state != -1) {
		// there should be at least one started changeset right now
		if ((*chs)->count <= 0) {
			knot_rrset_deep_free(&rr, 1, 1, 1);
			ret = KNOT_EMALF;
			goto cleanup;
		}
		
		// a changeset should be created only when there is a SOA
		assert((*chs)->sets[(*chs)->count - 1].soa_from != NULL);
		
		if ((*chs)->sets[(*chs)->count - 1].soa_to == NULL) {
			state = XFRIN_CHANGESET_REMOVE;
		} else {
			state = XFRIN_CHANGESET_ADD;
		}
	}
	
	/*! \todo This may be implemented with much less IFs! */
	
	while (ret == KNOT_EOK && rr != NULL) {
		switch (state) {
		case -1:
			// a SOA is expected
			// this may be either a start of a changeset or the
			// last SOA (in case the transfer was empty, but that
			// is quite weird in fact
			if (knot_rrset_type(rr) != KNOT_RRTYPE_SOA) {
				debug_knot_xfr("First RR is not a SOA RR!\n");
				ret = KNOT_EMALF;
				knot_rrset_deep_free(&rr, 1, 1, 1);
				goto cleanup;
			}
			
			if (knot_rdata_soa_serial(knot_rrset_rdata(rr))
			    == knot_rdata_soa_serial(
			           knot_rrset_rdata((*chs)->first_soa))) {
				// last SOA, discard and end
				knot_rrset_deep_free(&rr, 1, 1, 1);
				return 1;
			} else {
				// normal SOA, start new changeset
				if ((ret = knot_changesets_check_size(*chs))
				     != KNOT_EOK) {
					knot_rrset_deep_free(&rr, 1, 1, 1);
					goto cleanup;
				}
				
				(*chs)->count++;
				
				ret = knot_changeset_add_soa(
					&(*chs)->sets[(*chs)->count - 1], rr, 
					XFRIN_CHANGESET_REMOVE);
				if (ret != KNOT_EOK) {
					knot_rrset_deep_free(&rr, 1, 1, 1);
					goto cleanup;
				}
				
				// change state to REMOVE
				state = XFRIN_CHANGESET_REMOVE;
			}
			break;
		case XFRIN_CHANGESET_REMOVE:
			// if the next RR is SOA, store it and change state to
			// ADD
			if (knot_rrset_type(rr) == KNOT_RRTYPE_SOA) {
				// we should not be here if soa_from is not set
				assert((*chs)->sets[(*chs)->count - 1].soa_from
				       != NULL);
				
				ret = knot_changeset_add_soa(
					&(*chs)->sets[(*chs)->count - 1], rr, 
					XFRIN_CHANGESET_ADD);
				if (ret != KNOT_EOK) {
					knot_rrset_deep_free(&rr, 1, 1, 1);
					goto cleanup;
				}
				
				state = XFRIN_CHANGESET_ADD;
			} else {
				// just add the RR to the REMOVE part and
				// continue
				if ((ret = knot_changeset_add_new_rr(
				         &(*chs)->sets[(*chs)->count - 1], rr,
				         XFRIN_CHANGESET_REMOVE)) != KNOT_EOK) {
					knot_rrset_deep_free(&rr, 1, 1, 1);
					goto cleanup;
				}
			}
			break;
		case XFRIN_CHANGESET_ADD:
			// if the next RR is SOA change to state -1 and do not
			// parse next RR
			if (knot_rrset_type(rr) == KNOT_RRTYPE_SOA) {
				state = -1;
				continue;
			} else {
				// just add the RR to the ADD part and continue
				if ((ret = knot_changeset_add_new_rr(
				            &(*chs)->sets[(*chs)->count], rr,
				            XFRIN_CHANGESET_ADD)) != KNOT_EOK) {
					knot_rrset_deep_free(&rr, 1, 1, 1);
					goto cleanup;
				}
			}
			break;
		}
		
		// parse the next RR
		ret = knot_packet_parse_next_rr_answer(packet, &rr);
	}
	
	// here no RRs remain in the packet but the transfer is not finished
	// yet, return EOK
	return KNOT_EOK;
	
	/*
	 * One iteration of this loop processes one changeset - starting with
	 */
//	while (ret == KNOT_EOK && rr != NULL) {
//		if (knot_rrset_type(rr) != KNOT_RRTYPE_SOA) {
//			debug_knot_xfr("Next RR is not a SOA RR as it should be"
//			               "!\n");
//			ret = KNOT_EMALF;
//			goto cleanup;
//		}

//		if (knot_rdata_soa_serial(knot_rrset_rdata(rr))
//		    == knot_rdata_soa_serial(knot_rrset_rdata(soa1))) {
//			soa2 = rr;
//			debug_knot_xfr("IXFR/IN packet is parsed, first SOA serial"
//				       " matches current, chset count = %zu\n",
//				       (*chs)->count);
//			break;
//		}

//		if ((ret = knot_changesets_check_size(*chs))
//		     != KNOT_EOK) {
//			knot_rrset_deep_free(&rr, 1, 1, 1);
//			goto cleanup;
//		}

//		// save the origin SOA of the remove part
//		debug_knot_xfr("Processing IXFR/IN changeset #%zu\n", (*chs)->count);
//		ret = knot_changeset_add_soa(
//			&(*chs)->sets[(*chs)->count], rr, XFRIN_CHANGESET_REMOVE);
//		if (ret != KNOT_EOK) {
//			knot_rrset_deep_free(&rr, 1, 1, 1);
//			goto cleanup;
//		}

//		ret = knot_packet_parse_next_rr_answer(packet, &rr);
//		while (ret == KNOT_EOK && rr != NULL) {
//			if (knot_rrset_type(rr) == KNOT_RRTYPE_SOA) {
//				break;
//			}

//			assert(knot_rrset_type(rr) != KNOT_RRTYPE_SOA);
//			if ((ret = knot_changeset_add_new_rr(
//				     &(*chs)->sets[(*chs)->count], rr,
//				     XFRIN_CHANGESET_REMOVE)) != KNOT_EOK) {
//				knot_rrset_deep_free(&rr, 1, 1, 1);
//				goto cleanup;
//			}

//			ret = knot_packet_parse_next_rr_answer(packet, &rr);
//		}

//		if (rr == NULL || knot_rrset_type(rr) != KNOT_RRTYPE_SOA) {
//			debug_knot_xfr("Malformed IXFR packet.\n");
//			ret = KNOT_EMALF;
//			goto cleanup;
//		}

//		assert(rr != NULL
//		       && knot_rrset_type(rr) == KNOT_RRTYPE_SOA);

//		// save the origin SOA of the add part
//		ret = knot_changeset_add_soa(
//			&(*chs)->sets[(*chs)->count], rr, XFRIN_CHANGESET_ADD);
//		if (ret != KNOT_EOK) {
//			knot_rrset_deep_free(&rr, 1, 1, 1);
//			goto cleanup;
//		}

//		ret = knot_packet_parse_next_rr_answer(packet, &rr);
//		while (ret == KNOT_EOK && rr != NULL) {
//			if (knot_rrset_type(rr) == KNOT_RRTYPE_SOA) {
//				break;
//			}

//			assert(knot_rrset_type(rr) != KNOT_RRTYPE_SOA);
//			if ((ret = knot_changeset_add_new_rr(
//				     &(*chs)->sets[(*chs)->count], rr,
//				     XFRIN_CHANGESET_ADD)) != KNOT_EOK) {
//				knot_rrset_deep_free(&rr, 1, 1, 1);
//				goto cleanup;
//			}

//			ret = knot_packet_parse_next_rr_answer(packet, &rr);
//		}

//		if (rr == NULL || knot_rrset_type(rr) != KNOT_RRTYPE_SOA) {
//			debug_knot_xfr("Malformed IXFR packet.\n");
//			ret = KNOT_EMALF;
//			goto cleanup;
//		}

//		// next chunk, continue the whole loop
//		++(*chs)->count;
//	}

//	if (ret != KNOT_EOK) {
//		debug_knot_xfr("Could not parse next Answer RR: %s.\n",
//		               knot_strerror(ret));
//		ret = KNOT_EMALF;
//		goto cleanup;
//	}

//	/*! \todo Replace by checks? */
//	assert(soa2 != NULL);
//	assert(knot_rrset_type(soa2) == KNOT_RRTYPE_SOA);
//	assert(knot_rdata_soa_serial(knot_rrset_rdata(soa1))
//	       == knot_rdata_soa_serial(knot_rrset_rdata(soa2)));

//	knot_rrset_deep_free(&soa2, 1, 1, 1);

//	/*! \todo Determine finished transfer. */
//	debug_knot_xfr("xfrin_process_ixfr_packet() finished, "
//		       "count = %zu, ret = %d\n", (*chs)->count, 1);
//	return 1;

cleanup:
	debug_knot_xfr("Cleanup after processing IXFR/IN packet.\n");
	knot_free_changesets(chs);
	knot_packet_free(&packet);
	return ret;
}

/*----------------------------------------------------------------------------*/
/* Applying changesets to zone                                                */
/*----------------------------------------------------------------------------*/

typedef struct {
	/*!
	 * Deleted (without owners and RDATA) after successful update.
	 */
	knot_rrset_t **old_rrsets;
	int old_rrsets_count;
	int old_rrsets_allocated;

	/*!
	 * Deleted after successful update.
	 */
	knot_rdata_t *old_rdata;

	/*!
	 * \brief Copied RRSets (i.e. modified by the update).
	 *
	 * Deleted (without owners and RDATA) after failed update.
	 */
	knot_rrset_t **new_rrsets;
	int new_rrsets_count;
	int new_rrsets_allocated;

	/*!
	 * Deleted (without contents) after successful update.
	 */
	knot_node_t **old_nodes;
	int old_nodes_count;
	int old_nodes_allocated;

	/*!
	 * Deleted (without contents) after failed update.
	 */
	knot_node_t **new_nodes;
	int new_nodes_count;
	int new_nodes_allocated;
	
	ck_hash_table_item_t **old_hash_items;
	int old_hash_items_count;
	int old_hash_items_allocated;
} xfrin_changes_t;

/*----------------------------------------------------------------------------*/

static void xfrin_changes_free(xfrin_changes_t **changes)
{
	free((*changes)->old_nodes);
	free((*changes)->old_rrsets);
	free((*changes)->new_rrsets);
	free((*changes)->new_nodes);
	free((*changes)->old_hash_items);
}

/*----------------------------------------------------------------------------*/

static int xfrin_changes_check_rrsets(knot_rrset_t ***rrsets,
                                      int *count, int *allocated, int to_add)
{
	int new_count = *allocated * 2;
	if (*allocated == 0) {
		new_count = (*count + to_add); /* Prevent infinite loop. */
	}

	while (*count + to_add > new_count) {
		new_count += *allocated;
	}

	knot_rrset_t **rrsets_new =
		(knot_rrset_t **)calloc(new_count, sizeof(knot_rrset_t *));
	if (rrsets_new == NULL) {
		return KNOT_ENOMEM;
	}

	knot_rrset_t **rrsets_old = *rrsets;
	memcpy(rrsets_new, *rrsets, (*count) * sizeof(knot_rrset_t *));
	*rrsets = rrsets_new;
	*allocated = new_count;
	free(rrsets_old);

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_changes_check_nodes(knot_node_t ***nodes,
                                     int *count, int *allocated)
{
	/* Prevent infinite loop in case of allocated = 0. */
	int new_count = 0;
	if (*allocated == 0) {
		new_count = *count + 1;
	} else {
		if (*count == *allocated) {
			new_count = *allocated * 2;
		}
	}

	const size_t node_len = sizeof(knot_node_t *);
	knot_node_t **nodes_new = malloc(new_count * node_len);
	if (nodes_new == NULL) {
		return KNOT_ENOMEM;
	}

	memset(nodes_new, 0, new_count * node_len);
	memcpy(nodes_new, *nodes, (*count) * node_len);
	*nodes = nodes_new;
	*allocated = new_count;

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_changes_check_hash_items(ck_hash_table_item_t ***items,
                                          int *count, int *allocated)
{
	/* Prevent infinite loop in case of allocated = 0. */
	int new_count = 0;
	if (*allocated == 0) {
		new_count = *count + 1;
	} else {
		if (*count == *allocated) {
			new_count = *allocated * 2;
		}
	}

	const size_t item_len = sizeof(ck_hash_table_item_t *);
	ck_hash_table_item_t **items_new = malloc(new_count * item_len);
	if (items_new == NULL) {
		return KNOT_ENOMEM;
	}

	memset(items_new, 0, new_count * item_len);
	memcpy(items_new, *items, (*count) * item_len);
	*items = items_new;
	*allocated = new_count;

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static void xfrin_zone_contents_free(knot_zone_contents_t **contents)
{
	/*! \todo This should be all in some API!! */
	
	if ((*contents)->table != NULL) {
//		ck_destroy_table(&(*contents)->table, NULL, 0);
		ck_table_free(&(*contents)->table);
	}

	// free the zone tree, but only the structure
	// (nodes are already destroyed)
	debug_knot_zone("Destroying zone tree.\n");
	knot_zone_tree_free(&(*contents)->nodes);
	debug_knot_zone("Destroying NSEC3 zone tree.\n");
	knot_zone_tree_free(&(*contents)->nsec3_nodes);

	knot_nsec3_params_free(&(*contents)->nsec3_params);

	knot_dname_table_free(&(*contents)->dname_table);
	
	free(*contents);
	*contents = NULL;
}

/*----------------------------------------------------------------------------*/

static void xfrin_rollback_update(knot_zone_contents_t *contents,
                                  xfrin_changes_t *changes)
{
	/*
	 * This function is called only when no references were actually set to
	 * the new nodes, just the new nodes reference other.
	 * We thus do not need to fix any references, just from the old nodes
	 * to the new ones.
	 */

	// discard new nodes, but do not remove RRSets from them
	for (int i = 0; i < changes->new_nodes_count; ++i) {
		knot_node_free(&changes->new_nodes[i], 0, 0);
	}

	// set references from old nodes to new nodes to NULL and remove the
	// old flag
	for (int i = 0; i < changes->old_nodes_count; ++i) {
		knot_node_set_new_node(changes->old_nodes[i], NULL);
		knot_node_clear_old(changes->old_nodes[i]);
	}

	// discard new RRSets
	for (int i = 0; i < changes->old_rrsets_count; ++i) {
		knot_rrset_deep_free(&changes->new_rrsets[i], 0, 1, 0);
	}

	// destroy the shallow copy of zone
	xfrin_zone_contents_free(&contents);
}

/*----------------------------------------------------------------------------*/

static knot_rdata_t *xfrin_remove_rdata(knot_rrset_t *from,
                                        const knot_rrset_t *what)
{
	knot_rdata_t *old = NULL;
	knot_rdata_t *old_actual = NULL;

	const knot_rdata_t *rdata = knot_rrset_rdata(what);

	while (rdata != NULL) {
		old_actual = knot_rrset_remove_rdata(from, rdata);
		if (old_actual != NULL) {
			old_actual->next = old;
			old = old_actual;
		}
		rdata = knot_rrset_rdata_next(what, rdata);
	}

	return old;
}

/*----------------------------------------------------------------------------*/

static int xfrin_get_node_copy(knot_node_t **node, xfrin_changes_t *changes)
{
	knot_node_t *new_node =
		knot_node_get_new_node(*node);
	if (new_node == NULL) {
		debug_knot_xfr("Creating copy of node.\n");
		int ret = knot_node_shallow_copy(*node, &new_node);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to create node copy.\n");
			return KNOT_ENOMEM;
		}

		// save the copy of the node
		ret = xfrin_changes_check_nodes(
			&changes->new_nodes,
			&changes->new_nodes_count,
			&changes->new_nodes_allocated);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add new node to list.\n");
			knot_node_free(&new_node, 0, 0);
			return ret;
		}

		// save the old node to list of old nodes
		ret = xfrin_changes_check_nodes(
			&changes->old_nodes,
			&changes->old_nodes_count,
			&changes->old_nodes_allocated);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add old node to list.\n");
			knot_node_free(&new_node, 0, 0);
			return ret;
		}

		changes->new_nodes[changes->new_nodes_count++] = new_node;
		changes->old_nodes[changes->old_nodes_count++] = *node;

		// mark the old node as old
		knot_node_set_old(*node);

		knot_node_set_new(new_node);
		knot_node_set_new_node(*node, new_node);
	}

	*node = new_node;
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_copy_old_rrset(knot_rrset_t *old, knot_rrset_t **copy,
                                xfrin_changes_t *changes)
{
	// create new RRSet by copying the old one
	int ret = knot_rrset_shallow_copy(old, copy);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to create RRSet copy.\n");
		return KNOT_ENOMEM;
	}

	// add the RRSet to the list of new RRSets
	ret = xfrin_changes_check_rrsets(&changes->new_rrsets,
	                                 &changes->new_rrsets_count,
	                                 &changes->new_rrsets_allocated, 1);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to add new RRSet to list.\n");
		knot_rrset_free(copy);
		return ret;
	}

	changes->new_rrsets[changes->new_rrsets_count++] = *copy;

	// add the old RRSet to the list of old RRSets
	ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
	                                 &changes->old_rrsets_count,
	                                 &changes->old_rrsets_allocated, 1);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to add old RRSet to list.\n");
		return ret;
	}

	changes->old_rrsets[changes->old_rrsets_count++] = old;

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_copy_rrset(knot_node_t *node, knot_rr_type_t type,
                            knot_rrset_t **rrset, xfrin_changes_t *changes)
{
	knot_rrset_t *old = knot_node_remove_rrset(node, type);

	if (old == NULL) {
		debug_knot_xfr("RRSet not found for RR to be removed.\n");
		return 1;
	}

	int ret = xfrin_copy_old_rrset(old, rrset, changes);
	if (ret != KNOT_EOK) {
		return ret;
	}
	
	// replace the RRSet in the node copy by the new one
	ret = knot_node_add_rrset(node, *rrset, 0);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to add RRSet copy to node\n");
		return KNOT_ERROR;
	}
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_remove_rrsigs(xfrin_changes_t *changes,
                                     const knot_rrset_t *remove,
                                     knot_node_t *node,
                                     knot_rrset_t **rrset)
{
	assert(changes != NULL);
	assert(remove != NULL);
	assert(node != NULL);
	assert(rrset != NULL);
	assert(knot_rrset_type(remove) == KNOT_RRTYPE_RRSIG);
	
	/*! \todo These optimalizations may be useless as there may be only
	 *        one RRSet of each type and owner in the changeset.
	 */
	
	int ret;

	if (!*rrset
	    || knot_dname_compare(knot_rrset_owner(*rrset),
	                            knot_node_owner(node)) != 0
	    || knot_rrset_type(*rrset) != knot_rdata_rrsig_type_covered(
	                  knot_rrset_rdata(remove))) {
		// find RRSet based on the Type Covered
		knot_rr_type_t type = knot_rdata_rrsig_type_covered(
			knot_rrset_rdata(remove));
		
		// copy the rrset
		ret = xfrin_copy_rrset(node, type, rrset, changes);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to copy rrset from changeset.\n");
			return ret;
		}
	} else {
		// we should have the right RRSIG RRSet in *rrset
		assert(knot_rrset_type(*rrset) 
		       == knot_rdata_rrsig_type_covered(
		                 knot_rrset_rdata(remove)));
		// this RRSet should be the already copied RRSet so we may
		// update it right away
	}
	
	// get the old rrsigs
	knot_rrset_t *old = knot_rrset_get_rrsigs(*rrset);
	if (old == NULL) {
		return 1;
	}
	
	// copy the RRSIGs
	/*! \todo This may be done unnecessarily more times. */
	knot_rrset_t *rrsigs;
	ret = xfrin_copy_old_rrset(old, &rrsigs, changes);
	if (ret != KNOT_EOK) {
		return ret;
	}
	
	// set the RRSIGs to the new RRSet copy
	if (knot_rrset_set_rrsigs(*rrset, rrsigs) != KNOT_EOK) {
		debug_knot_xfr("Failed to set rrsigs.\n");
		return KNOT_ERROR;
	}
	
	

	// now in '*rrset' we have a copy of the RRSet which holds the RRSIGs 
	// and in 'rrsigs' we have the copy of the RRSIGs
	
	knot_rdata_t *rdata = xfrin_remove_rdata(rrsigs, remove);
	if (rdata == NULL) {
		debug_knot_xfr("Failed to remove RDATA from RRSet: %s.\n",
		               knot_strerror(ret));
		return 1;
	}
	
	// if the RRSet is empty, remove from node and add to old RRSets
	// check if there is no RRSIGs; if there are, leave the RRSet
	// there; it may be eventually removed when the RRSIGs are removed
	if (knot_rrset_rdata(rrsigs) == NULL) {
		// remove the RRSIGs from the RRSet
		knot_rrset_set_rrsigs(*rrset, NULL);
		
		ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
		                                 &changes->old_rrsets_count,
		                                 &changes->old_rrsets_allocated,
		                                 1);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add empty RRSet to the "
			               "list of old RRSets.");
			// delete the RRSet right away
			knot_rrset_free(&rrsigs);
			return ret;
		}
	
		changes->old_rrsets[changes->old_rrsets_count++] = rrsigs;
		
		// now check if the RRSet is not totally empty
		if (knot_rrset_rdata(*rrset) == NULL) {
			assert(knot_rrset_rrsigs(*rrset) == NULL);
			
			// remove the whole RRSet from the node
			knot_rrset_t *tmp = knot_node_remove_rrset(node,
			                             knot_rrset_type(*rrset));
			assert(tmp == *rrset);
			
			ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
			                        &changes->old_rrsets_count,
			                        &changes->old_rrsets_allocated,
			                        1);
			if (ret != KNOT_EOK) {
				debug_knot_xfr("Failed to add empty RRSet to "
				               "the list of old RRSets.");
				// delete the RRSet right away
				knot_rrset_free(rrset);
				return ret;
			}
		
			changes->old_rrsets[changes->old_rrsets_count++] = 
				*rrset;
		}
	}
	
	// connect the RDATA to the list of old RDATA
	rdata->next = changes->old_rdata;
	changes->old_rdata = rdata;
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_remove_normal(xfrin_changes_t *changes,
                                     const knot_rrset_t *remove,
                                     knot_node_t *node,
                                     knot_rrset_t **rrset)
{
	assert(changes != NULL);
	assert(remove != NULL);
	assert(node != NULL);
	assert(rrset != NULL);
	
	int ret;
	
	// now we have the copy of the node, so lets get the right RRSet
	// check if we do not already have it
	if (!*rrset
	    || knot_dname_compare(knot_rrset_owner(*rrset),
	                          knot_node_owner(node)) != 0
	    || knot_rrset_type(*rrset)
	       != knot_rrset_type(remove)) {
		/*!
		 * \todo This may happen also with already 
		 *       copied RRSet. In that case it would be
		 *       an unnecesary overhead but will 
		 *       probably not cause problems. TEST!!
		 */
		ret = xfrin_copy_rrset(node,
			knot_rrset_type(remove), rrset, changes);
		if (ret != KNOT_EOK) {
			return ret;
		}
	}
	
	if (*rrset == NULL) {
		debug_knot_xfr("RRSet not found for RR to be removed.\n");
		return 1;
	}
	
DEBUG_KNOT_XFR(
	char *name = knot_dname_to_str(knot_rrset_owner(*rrset));
	debug_knot_xfr("Updating RRSet with owner %s, type %s\n", name,
		  knot_rrtype_to_string(knot_rrset_type(*rrset)));
	free(name);
);

	// remove the specified RRs from the RRSet (de facto difference of
	// sets)
	knot_rdata_t *rdata = xfrin_remove_rdata(*rrset, remove);
	if (rdata == NULL) {
		debug_knot_xfr("Failed to remove RDATA from RRSet: %s.\n",
			  knot_strerror(ret));
		return 1;
	}
	
	// if the RRSet is empty, remove from node and add to old RRSets
	// check if there is no RRSIGs; if there are, leave the RRSet
	// there; it may be eventually removed when the RRSIGs are removed
	if (knot_rrset_rdata(*rrset) == NULL
	    && knot_rrset_rrsigs(*rrset) == NULL) {
		
		knot_rrset_t *tmp = knot_node_remove_rrset(node,
		                                     knot_rrset_type(*rrset));
		
		// add the removed RRSet to list of old RRSets
		
		assert(tmp == *rrset);
		ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
		                                 &changes->old_rrsets_count,
		                                 &changes->old_rrsets_allocated,
		                                 1);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add empty RRSet to the "
			          "list of old RRSets.");
			// delete the RRSet right away
			knot_rrset_free(rrset);
			return ret;
		}
	
		changes->old_rrsets[changes->old_rrsets_count++] = *rrset;
	}
	
	// connect the RDATA to the list of old RDATA
	rdata->next = changes->old_rdata;
	changes->old_rdata = rdata;
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_remove_all_rrsets(xfrin_changes_t *changes,
                                         knot_node_t *node, uint16_t type)
{
	/*! \todo Implement. */
	int ret;

	if (type == KNOT_RRTYPE_ANY) {
		// put all the RRSets to the changes structure
		ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
		                                 &changes->old_rrsets_count,
		                                 &changes->old_rrsets_allocated,
		                                 knot_node_rrset_count(node));
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to check changeset rrsets.\n");
			return ret;
		}

		knot_rrset_t **rrsets = knot_node_get_rrsets(node);
		knot_rrset_t **place = changes->old_rrsets
		                       + changes->old_rrsets_count;
		/*! \todo Test this!!! */
		memcpy(place, rrsets, knot_node_rrset_count(node) * sizeof(knot_rrset_t *));

		// remove all RRSets from the node
		knot_node_remove_all_rrsets(node);
	} else {
		ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
		                                 &changes->old_rrsets_count,
		                                 &changes->old_rrsets_allocated,
		                                 1);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to check changeset rrsets.\n");
			return ret;
		}
		// remove only RRSet with the given type
		knot_rrset_t *rrset = knot_node_remove_rrset(node, type);
		changes->old_rrsets[changes->old_rrsets_count++] = rrset;
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_remove(knot_zone_contents_t *contents,
                              knot_changeset_t *chset,
                              xfrin_changes_t *changes)
{
	/*
	 * Iterate over removed RRSets, copy appropriate nodes and remove
	 * the rrsets from them. By default, the RRSet should be copied so that
	 * RDATA may be removed from it.
	 */
	int ret = 0;
	knot_node_t *node = NULL;
	knot_rrset_t *rrset = NULL;

	for (int i = 0; i < chset->remove_count; ++i) {
		// check if the old node is not the one we should use
		if (!node || knot_rrset_owner(chset->remove[i])
		             != knot_node_owner(node)) {
			node = knot_zone_contents_get_node(contents,
			                  knot_rrset_owner(chset->remove[i]));
			if (node == NULL) {
				debug_knot_xfr("Node not found for RR to be removed"
				          "!\n");
				continue;
			}
		}

		// create a copy of the node if not already created
		if (!knot_node_is_new(node)) {
			ret = xfrin_get_node_copy(&node, changes);
			if (ret != KNOT_EOK) {
				return ret;
			}
		}

		assert(node != NULL);
		assert(knot_node_is_new(node));
		
		// first check if all RRSets should be removed
		if (knot_rrset_class(chset->remove[i]) == KNOT_CLASS_ANY) {
			ret = xfrin_apply_remove_all_rrsets(
				changes, node,
				knot_rrset_type(chset->remove[i]));
		} else if (knot_rrset_type(chset->remove[i])
		           == KNOT_RRTYPE_RRSIG) {
			// this should work also for UPDATE
			ret = xfrin_apply_remove_rrsigs(changes,
			                                chset->remove[i],
			                                node, &rrset);
		} else {
			// this should work also for UPDATE
			ret = xfrin_apply_remove_normal(changes,
			                                chset->remove[i],
			                                node, &rrset);
		}
		
		if (ret > 0) {
			debug_knot_xfr("xfrin_apply_remove() ret = %d, "
				       "continuing.\n", ret);
			continue;
		} else if (ret != KNOT_EOK) {
			debug_knot_xfr("xfrin_apply_remove() failed - %s.\n",
				       knot_strerror(ret));
			return ret;
		}
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static knot_node_t *xfrin_add_new_node(knot_zone_contents_t *contents,
                                       knot_rrset_t *rrset)
{
	/*! \todo Why is the function disabled? */
	//return NULL;

	knot_node_t *node = knot_node_new(knot_rrset_get_owner(rrset),
	                                  NULL, KNOT_NODE_FLAGS_NEW);
	if (node == NULL) {
		debug_knot_xfr("Failed to create a new node.\n");
		return NULL;
	}

	int ret = 0;

	// insert the node into zone structures and create parents if
	// necessary
	if (knot_rrset_type(rrset) == KNOT_RRTYPE_NSEC3) {
		ret = knot_zone_contents_add_nsec3_node(contents, node, 1, 0,
		                                        1);
	} else {
		ret = knot_zone_contents_add_node(contents, node, 1,
		                                  KNOT_NODE_FLAGS_NEW, 1);
	}
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to add new node to zone contents.\n");
		return NULL;
	}

	// find previous node and connect the new one to it
	knot_node_t *prev = NULL;
	if (knot_rrset_type(rrset) == KNOT_RRTYPE_NSEC3) {
		prev = knot_zone_contents_get_previous_nsec3(contents,
		                                       knot_rrset_owner(rrset));
	} else {
		prev = knot_zone_contents_get_previous(contents,
		                                       knot_rrset_owner(rrset));
	}

	// fix prev and next pointers
	if (prev != NULL) {
		knot_node_set_previous(node, prev);
	}

	return node;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_add_normal(xfrin_changes_t *changes,
                                  knot_rrset_t *add,
                                  knot_node_t *node,
                                  knot_rrset_t **rrset)
{
	assert(changes != NULL);
	assert(remove != NULL);
	assert(node != NULL);
	assert(rrset != NULL);
	
	int ret;
	
	if (!*rrset
	    || knot_dname_compare(knot_rrset_owner(*rrset),
	                          knot_node_owner(node)) != 0
	    || knot_rrset_type(*rrset)
	       != knot_rrset_type(add)) {
		*rrset = knot_node_remove_rrset(node, knot_rrset_type(add));
	}

	if (*rrset == NULL) {
		debug_knot_xfr("RRSet to be added not found in zone.\n");
		// add the RRSet from the changeset to the node
		/*! \todo What about domain names?? Shouldn't we use the
		 *        zone-contents' version of this function??
		 */
		ret = knot_node_add_rrset(node, add, 0);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add RRSet to node.\n");
			return KNOT_ERROR;
		}
		return KNOT_EOK; // done, continue
	}

	knot_rrset_t *old = *rrset;

DEBUG_KNOT_XFR(
	char *name = knot_dname_to_str(knot_rrset_owner(*rrset));
	debug_knot_xfr("Found RRSet with owner %s, type %s\n", name,
	          knot_rrtype_to_string(knot_rrset_type(*rrset)));
	free(name);
);
	ret = xfrin_copy_old_rrset(old, rrset, changes);
	if (ret != KNOT_EOK) {
		return ret;
	}

	// merge the changeset RRSet to the copy
	/* What if the update fails?
	 * The changesets will be destroyed - that will destroy 'add',
	 * and the copied RRSet will be destroyed because it is in the new
	 * rrsets list.
	 *
	 * If the update is successfull, the old RRSet will be destroyed,
	 * but the one from the changeset will be not!!
	 *
	 * TODO: add the 'add' rrset to list of old RRSets?
	 */
	ret = knot_rrset_merge((void **)rrset, (void **)&add);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to merge changeset RRSet to copy.\n");
		return KNOT_ERROR;
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_add_rrsig(xfrin_changes_t *changes,
                                  knot_rrset_t *add,
                                  knot_node_t *node,
                                  knot_rrset_t **rrset)
{
	assert(changes != NULL);
	assert(remove != NULL);
	assert(node != NULL);
	assert(rrset != NULL);
	assert(knot_rrset_type(add) == KNOT_RRTYPE_RRSIG);
	
	int ret;
	
	knot_rr_type_t type = knot_rdata_rrsig_type_covered(
	                                               knot_rrset_rdata(add));
	
	if (!*rrset
	    || knot_dname_compare(knot_rrset_owner(*rrset),
	                          knot_node_owner(node)) != 0
	    || knot_rrset_type(*rrset) != knot_rdata_rrsig_type_covered(
	                                             knot_rrset_rdata(add))) {
		// copy the rrset
		ret = xfrin_copy_rrset(node, type, rrset, changes);
		if (ret < 0) {
			return ret;
		}
	} else {
		// we should have the right RRSIG RRSet in *rrset
		assert(knot_rrset_type(*rrset) == type);
		// this RRSet should be the already copied RRSet so we may
		// update it right away
	}

	if (*rrset == NULL) {
		debug_knot_xfr("RRSet to be added not found in zone.\n");
		
		// create a new RRSet to add the RRSIGs into
		*rrset = knot_rrset_new(knot_node_get_owner(node), type,
		                        knot_rrset_class(add),
		                        knot_rrset_ttl(add));
		if (*rrset == NULL) {
			debug_knot_xfr("Failed to create new RRSet for RRSIGs.\n");
			return KNOT_ENOMEM;
		}
		
		// add the RRSet from the changeset to the node
		ret = knot_node_add_rrset(node, *rrset, 0);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add RRSet to node.\n");
			return KNOT_ERROR;
		}
	}

DEBUG_KNOT_XFR(
		char *name = knot_dname_to_str(knot_rrset_owner(*rrset));
		debug_knot_xfr("Found RRSet with owner %s, type %s\n", name,
			  knot_rrtype_to_string(knot_rrset_type(*rrset)));
		free(name);
);

	if (knot_rrset_rrsigs(*rrset) == NULL) {
		ret = knot_rrset_set_rrsigs(*rrset, add);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to add RRSIGs to the RRSet.\n");
			return KNOT_ERROR;
		}
		
		return KNOT_EOK;
	} else {
		knot_rrset_t *old = knot_rrset_get_rrsigs(*rrset);
		assert(old != NULL);
		knot_rrset_t *rrsig;
		
		ret = xfrin_copy_old_rrset(old, &rrsig, changes);
		if (ret != KNOT_EOK) {
			return ret;
		}
		
		// replace the old RRSIGs with the new ones
		knot_rrset_set_rrsigs(*rrset, rrsig);
	
		// merge the changeset RRSet to the copy
		/*! \todo What if the update fails?
		 * 
		 */
		ret = knot_rrset_merge((void **)&rrsig, (void **)&add);
		if (ret != KNOT_EOK) {
			debug_knot_xfr("Failed to merge changeset RRSet to copy.\n");
			return KNOT_ERROR;
		}
	}
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_add(knot_zone_contents_t *contents,
                           knot_changeset_t *chset,
                           xfrin_changes_t *changes)
{
	// iterate over removed RRSets, copy appropriate nodes and remove
	// the rrsets from them
	int ret = 0;
	knot_node_t *node = NULL;
	knot_rrset_t *rrset = NULL;

	for (int i = 0; i < chset->add_count; ++i) {
		// check if the old node is not the one we should use
		if (!node || knot_rrset_owner(chset->add[i])
			     != knot_node_owner(node)) {
			node = knot_zone_contents_get_node(contents,
			                  knot_rrset_owner(chset->add[i]));
			if (node == NULL) {
				// create new node, connect it properly to the
				// zone nodes
				debug_knot_xfr("Creating new node.\n");
				node = xfrin_add_new_node(contents,
				                          chset->add[i]);
				if (node == NULL) {
					debug_knot_xfr("Failed to create new node "
					          "in zone.\n");
					return KNOT_ERROR;
				}
				continue; // continue with another RRSet
			}
		}

		// create a copy of the node if not already created
		if (!knot_node_is_new(node)) {
			xfrin_get_node_copy(&node, changes);
		}

		assert(node != NULL);
		assert(knot_node_is_new(node));
		
		if (knot_rrset_type(chset->add[i]) == KNOT_RRTYPE_RRSIG) {
			ret = xfrin_apply_add_rrsig(changes, chset->add[i],
			                            node, &rrset);
		} else {
			ret = xfrin_apply_add_normal(changes, chset->add[i],
			                             node, &rrset);
		}
		
		if (ret != KNOT_EOK) {
			return ret;
		}
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/
/*!
 * \todo This must be tested!! Simulate failure somehow.
 */
static void xfrin_clean_changes_after_fail(xfrin_changes_t *changes)
{
	/* 1) Delete copies of RRSets created because they were updated.
	 *    Do not delete their RDATA or owners.
	 */
	for (int i = 0; i < changes->new_rrsets_count; ++i) {
		knot_rrset_free(&changes->new_rrsets[i]);
	}

	/* 2) Delete copies of nodes created because they were updated.
	 *    Do not delete their RRSets.
	 */
	for (int i = 0; i < changes->new_nodes_count; ++i) {
		knot_node_free(&changes->new_nodes[i], 0, 1);
	}

	// changesets will be deleted elsewhere
	// so just delete the changes structure
	xfrin_changes_free(&changes);
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_replace_soa(knot_zone_contents_t *contents,
                                   xfrin_changes_t *changes,
                                   knot_changeset_t *chset)
{
	knot_node_t *node = knot_zone_contents_get_apex(contents);
	assert(node != NULL);

	int ret = 0;

	// create a copy of the node if not already created
	if (!knot_node_is_new(node)) {
		ret = xfrin_get_node_copy(&node, changes);
		if (ret != KNOT_EOK) {
			return ret;
		}
	}

	assert(knot_node_is_new(node));

	// remove the SOA RRSet from the apex
	knot_rrset_t *rrset = knot_node_remove_rrset(node, KNOT_RRTYPE_SOA);
	assert(rrset != NULL);

	// add the old RRSet to the list of old RRSets
	ret = xfrin_changes_check_rrsets(&changes->old_rrsets,
	                                 &changes->old_rrsets_count,
	                                 &changes->old_rrsets_allocated, 1);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to add old RRSet to list.\n");
		return ret;
	}

	changes->old_rrsets[changes->old_rrsets_count++] = rrset;

	// and just insert the new SOA RRSet to the node
	ret = knot_node_add_rrset(node, chset->soa_to, 0);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to add RRSet to node.\n");
		return KNOT_ERROR;
	}

	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_apply_changeset(knot_zone_contents_t *contents,
                                 xfrin_changes_t *changes,
                                 knot_changeset_t *chset)
{
	/*
	 * Applies one changeset to the zone. Checks if the changeset may be
	 * applied (i.e. the origin SOA (soa_from) has the same serial as
	 * SOA in the zone apex.
	 */

	// check if serial matches
	/*! \todo Only if SOA is present? */
	const knot_rrset_t *soa = knot_node_rrset(contents->apex,
	                                          KNOT_RRTYPE_SOA);
	if (soa == NULL || knot_rdata_soa_serial(knot_rrset_rdata(soa))
	                   != chset->serial_from) {
		debug_knot_xfr("SOA serials do not match!!\n");
		return KNOT_ERROR;
	}

	int ret = xfrin_apply_remove(contents, chset, changes);
	if (ret != KNOT_EOK) {
		xfrin_clean_changes_after_fail(changes);
		return ret;
	}

	ret = xfrin_apply_add(contents, chset, changes);
	if (ret != KNOT_EOK) {
		xfrin_clean_changes_after_fail(changes);
		return ret;
	}

	/*! \todo Only if SOA is present? */
	return xfrin_apply_replace_soa(contents, changes, chset);
}

/*----------------------------------------------------------------------------*/

static void xfrin_check_node_in_tree(knot_zone_tree_node_t *tnode, void *data)
{
	assert(tnode != NULL);
	assert(data != NULL);
	assert(tnode->node != NULL);
	
	xfrin_changes_t *changes = (xfrin_changes_t *)data;

	knot_node_t *node = knot_node_get_new_node(tnode->node);

	if (node == NULL) {
		// no RRSets were removed from this node, thus it cannot be
		// empty
		return;
	}

	debug_knot_xfr("xfrin_check_node_in_tree: children of old node: %u, "
		       "children of new node: %u.\n",
		       knot_node_children(node),
		       knot_node_children(tnode->node));

	
	// check if the node is empty and has no children
	// to be sure, check also the count of children of the old node
	if (knot_node_rrset_count(node) == 0
	    && knot_node_children(node) == 0
	    && knot_node_children(tnode->node) == 0) {
		// in this case the new node copy should be removed
		// but it cannot be deleted because if a rollback happens,
		// the node must be in the new nodes list
		// just add it to the old nodes list so that it is deleted
		// after successful update

		// set the new node of the old node to NULL
		knot_node_set_new_node(tnode->node, NULL);
		
		// if the parent has a new copy, decrease the number of
		// children of that copy
		if (knot_node_new_node(knot_node_parent(node, 0))) {
			/*! \todo Replace by some API. */
			--node->parent->new_node->children;
		}
		
		// put the new node to te list of old nodes
		if (xfrin_changes_check_nodes(&changes->old_nodes,
		                              &changes->old_nodes_count,
		                              &changes->old_nodes_allocated) 
			!= KNOT_EOK) {
			/*! \todo Notify about the error!!! */
			return;
		}
		
		changes->old_nodes[changes->old_nodes_count++] = node;
		
		// leave the old node in the old node list, we will delete
		// it later
	}
}

/*----------------------------------------------------------------------------*/

static int xfrin_finalize_remove_nodes(knot_zone_contents_t *contents,
                                       xfrin_changes_t *changes)
{
	assert(contents != NULL);
	assert(changes != NULL);
	
	knot_node_t *node;
	knot_zone_tree_node_t *removed;
	ck_hash_table_item_t *rem_hash;
	int ret;
	
	for (int i = 0; i < changes->old_nodes_count; ++i) {
		node = changes->old_nodes[i];

		// if the node is marked as old and has no new node copy
		// remove it from the zone structure but do not delete it
		// that may be done only after the grace period
		if (knot_node_is_old(node) 
		    && knot_node_new_node(node) == NULL) {
		
			if (knot_node_rrset(node, KNOT_RRTYPE_NSEC3) 
			    != NULL) {
				ret = knot_zone_contents_remove_nsec3_node(
					contents, node, &removed);
			} else {
				ret = knot_zone_contents_remove_node(
					contents, node, &removed, &rem_hash);
			}
			if (ret != KNOT_EOK) {
				debug_knot_xfr("Failed to remove node from zone"
				               "!\n");
				return KNOT_ENONODE;
			}
			
			assert(removed != NULL);
			assert(removed->node == node);
			// delete the tree node (not needed)
			free(removed);
			
			if (rem_hash != NULL) {
				// save the removed hash table item
				ret = xfrin_changes_check_hash_items(
					&changes->old_hash_items,
					&changes->old_hash_items_count,
					&changes->old_hash_items_allocated);
				if (ret != KNOT_EOK) {
					debug_knot_xfr("Failed to save the hash"
					               " table item to list of "
					               "old items.\n");
					return ret;
				}
				changes->old_hash_items[
					changes->old_hash_items_count++]
					= rem_hash;
			}
		}
	}
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_finalize_contents(knot_zone_contents_t *contents,
                                   xfrin_changes_t *changes)
{
	// don't know what should have been done here, except for one thing:
	// walk through the zone and remove empty nodes (save them in the
	// old nodes list). But only those having no children!!!
	
	/*
	 * Walk through the zone and remove empty nodes.
	 * We must walk backwards, so that children are processed before
	 * their parents. This will allow to remove chain of parent-children
	 * nodes.
	 * We cannot remove the nodes right away as it would modify the very
	 * structure used for walking through the zone. Just put the nodes
	 * to the list of old nodes to be removed.
	 * We must also decrease the node's parent's children count now
	 * and not when deleting the node, so that the chain of parent-child
	 * nodes may be removed.
	 */
	knot_zone_tree_t *t = knot_zone_contents_get_nodes(contents);
	assert(t != NULL);
	
	// walk through the zone and select nodes to be removed
	knot_zone_tree_reverse_apply_postorder(t, xfrin_check_node_in_tree, 
	                                       (void *)changes);
	
	// Do the same with NSEC3 nodes.
	t = knot_zone_contents_get_nsec3_nodes(contents);
	assert(t != NULL);
	
	knot_zone_tree_reverse_apply_postorder(t, xfrin_check_node_in_tree, 
	                                       (void *)changes);
	
	// remove the nodes one by one
	return xfrin_finalize_remove_nodes(contents, changes);
}

/*----------------------------------------------------------------------------*/

static void xfrin_fix_refs_in_node(knot_zone_tree_node_t *tnode, void *data)
{
	/*! \todo Passed data is always seto to NULL. */
	assert(tnode != NULL);
	//assert(data != NULL);

	//xfrin_changes_t *changes = (xfrin_changes_t *)data;

	// 1) Fix the reference to the node to the new one if there is some
	knot_node_t *node = tnode->node;

	knot_node_t *new_node = knot_node_get_new_node(node);
	if (new_node != NULL) {
		//assert(knot_node_rrset_count(new_node) > 0);
		node = new_node;
		tnode->node = new_node;
	}

	// 2) fix references from the node remaining in the zone
	knot_node_update_refs(node);
}

/*----------------------------------------------------------------------------*/

static void xfrin_fix_gen_in_node(knot_zone_tree_node_t *tnode, void *data)
{
	/*! \todo Passed data is always seto to NULL. */
	assert(tnode != NULL);
	
	knot_node_t *node = tnode->node;

	knot_node_set_old(node);
}

/*----------------------------------------------------------------------------*/

static void xfrin_fix_hash_refs(ck_hash_table_item_t *item, void *data)
{
	if (item == NULL) {
		return;
	}
	
	knot_node_t *new_node = knot_node_get_new_node(
	                             (knot_node_t *)item->value);
	if (new_node != NULL) {
		assert(item->key_length
		       == knot_dname_size(knot_node_owner(new_node)));
		assert(strncmp(item->key, (const char *)knot_dname_name(
		            knot_node_owner(new_node)), item->key_length) == 0);
		item->value = (void *)new_node;
		item->key = (const char *)knot_dname_name(
		                             knot_node_owner(new_node));
	}
}

/*----------------------------------------------------------------------------*/

static void xfrin_fix_dname_refs(knot_dname_t *dname, void *data)
{
	UNUSED(data);
	knot_dname_update_node(dname);
}

/*----------------------------------------------------------------------------*/

static int xfrin_fix_references(knot_zone_contents_t *contents)
{
	/*! \todo This function must not fail!! */

	/*
	 * Now the contents are already switched, and we should update all
	 * references not updated yet, so that the old contents may be removed.
	 *
	 * Walk through the zone tree, so that each node will be checked
	 * and updated.
	 */
	// fix references in normal nodes
	knot_zone_tree_t *tree = knot_zone_contents_get_nodes(contents);
	knot_zone_tree_forward_apply_inorder(tree, xfrin_fix_refs_in_node,
	                                     NULL);

	// fix refereces in NSEC3 nodes
	tree = knot_zone_contents_get_nsec3_nodes(contents);
	knot_zone_tree_forward_apply_inorder(tree, xfrin_fix_refs_in_node,
	                                     NULL);

	// fix references in hash table
	ck_hash_table_t *table = knot_zone_contents_get_hash_table(contents);
	ck_apply(table, xfrin_fix_hash_refs, NULL);
	
	// fix references dname table
	int ret = knot_zone_contents_dname_table_apply(contents,
	                                            xfrin_fix_dname_refs, NULL);
	assert(ret == KNOT_EOK);
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static int xfrin_fix_generation(knot_zone_contents_t *contents) 
{
	assert(contents != NULL);
	
	knot_zone_tree_t *tree = knot_zone_contents_get_nodes(contents);
	knot_zone_tree_forward_apply_inorder(tree, xfrin_fix_gen_in_node,
	                                     NULL);
	
	tree = knot_zone_contents_get_nsec3_nodes(contents);
	knot_zone_tree_forward_apply_inorder(tree, xfrin_fix_gen_in_node,
	                                     NULL);
	
	return KNOT_EOK;
}

/*----------------------------------------------------------------------------*/

static void xfrin_cleanup_update(xfrin_changes_t *changes)
{
	// free old nodes but do not destroy their RRSets
	// remove owners also, because of reference counting
	for (int i = 0; i < changes->old_nodes_count; ++i) {
		knot_node_free(&changes->old_nodes[i], 1, 0);
	}

	// free old RRSets, and destroy also domain names in them
	// because of reference counting
	for (int i = 0; i < changes->old_rrsets_count; ++i) {
		knot_rrset_deep_free(&changes->old_rrsets[i], 0, 1, 1);
	}
	
	// free old hash table items, but do not touch their contents
	for (int i = 0; i < changes->old_hash_items_count; ++i) {
		free(changes->old_hash_items[i]);
	}
}

/*----------------------------------------------------------------------------*/

int xfrin_apply_changesets_to_zone(knot_zone_t *zone, 
                                   knot_changesets_t *chsets)
{
	if (zone == NULL || chsets == NULL || chsets->count == 0) {
		return KNOT_EBADARG;
	}
	
	knot_zone_contents_t *old_contents = knot_zone_get_contents(zone);
	
//	debug_knot_xfr("\nOLD ZONE CONTENTS:\n\n");
//	knot_zone_contents_dump(old_contents, 1);

	/*
	 * Ensure that the zone generation is set to 0.
	 */
	if (!knot_zone_contents_gen_is_old(old_contents)) {
		// this would mean that a previous update was not completed
		// abort
		debug_knot_zone("Trying to apply changesets to zone that is "
		                  "being updated. Aborting.\n");
		return KNOT_EAGAIN;
	}

	/*
	 * Create a shallow copy of the zone, so that the structures may be
	 * updated.
	 *
	 * This will create new zone contents structures (normal nodes' tree,
	 * NSEC3 tree, hash table, domain name table), but fill them with the
	 * data from the old contents.
	 */
	knot_zone_contents_t *contents_copy = NULL;

	int ret = knot_zone_contents_shallow_copy(old_contents,
	                                          &contents_copy);
	if (ret != KNOT_EOK) {
		debug_knot_xfr("Failed to create shallow copy of zone: %s\n",
		          knot_strerror(ret));
		return ret;
	}

	/*
	 * Now, apply one changeset after another until all are applied.
	 * Changesets may be either from IXFR or from a dynamic update. 
	 * Dynamic updates use special TYPE and CLASS values to distinguish
	 * requests, such as "remove all RRSets from a node", "remove all RRs
	 * with the specified type from a node", etc.
	 *
	 * When updating anything within some node (removing RR, adding RR),
	 * the node structure is copied, but the RRSets within are not.
	 *
	 *   1) When removing RRs from node, The affected RRSet is copied. This
	 *      it also a 'shallow copy', i.e. the RDATA remain the exact same.
	 *      The specified RRs (i.e. RDATA) are then removed from the copied
	 *      RRSet.
	 *   2) When adding RRs to node, there are two cases:
	 *      a) If there is a RRSet that should contain these RRs
	 *         this RRSet is copied (shallow copy) and the RRs are added to
	 *         it (rrset_merge()).
	 *      b) If there is not such a RRSet, the whole RRSet from the 
	 *         changeset is added to the new node (thus this RRSet must not
	 *         be deleted afterwards).
	 *
	 * A special case are RRSIG RRs. These functions assume that they 
	 * are grouped together in knot_rrset_t structures according to
	 * their header (owner, type, class) AND their 'type covered', i.e.
	 * there may be more RRSIG RRSets in one changeset (while there
	 * should not be more RRSets of any other type).
	 *   3) When removing RRSIG RRs from node, the appropriate RRSet holding
	 *      them must be found (according to the 'type covered' field). This
	 *      RRSet is then copied (shallow copy), Its RRSIGs are also copied
	 *      and the RRSIG RRs are added to the RRSIG copy.
	 *   4) When adding RRSIG RRs to node, the same process is done - the 
	 *      proper RRSet holding them is found, copied, its RRSIGs are
	 *      copied (if there are some) and the RRs are added to the copy.
	 *
	 * When a node is copied, reference to the copy is stored within the
	 * old node (node_t.old_node). This is important, because when the
	 * zone contents are switched to the new ones, references from old nodes
	 * that should point to new nodes are not yet set (it would influence
	 * replying from the old zone contents). While all these references
	 * (such as node_t.prev, node_t.next, node_t.parent, etc.) are properly
	 * modified, the search functions use old or new nodes accordingly
	 * (old nodes while old contents are used, new nodes when new contents
	 * are used). The 'check_version' parameter turns on this behaviour in 
	 * search functions.
	 *
	 * In case of error, we must remove all data created by the update, i.e.
	 *   - new nodes,
	 *   - new RRSets,
	 * and remove the references to the new nodes from old nodes.
	 *
	 * In case of success, the RRSet structures from the changeset
	 * structures must not be deleted, as they are either already used by 
	 * the server (stored within the new zone contents) or deleted when
	 * cleaning up the temporary 'changes' structure.
	 */
	xfrin_changes_t changes;
	memset(&changes, 0, sizeof(xfrin_changes_t));

	for (int i = 0; i < chsets->count; ++i) {
		if ((ret = xfrin_apply_changeset(contents_copy, &changes,
		                               &chsets->sets[i])) != KNOT_EOK) {
			xfrin_rollback_update(contents_copy, &changes);
			debug_knot_xfr("Failed to apply changesets to zone: "
			          "%s\n", knot_strerror(ret));
			return ret;
		}
	}

	/*
	 * When all changesets are applied, set generation 1 to the copy of
	 * the zone so that new nodes are used instead of old ones.
	 */
//	knot_zone_contents_switch_generation(contents_copy);
	//contents_copy->generation = 1;
	knot_zone_contents_set_gen_new(contents_copy);

	/*
	 * Finalize the zone contents.
	 */
	ret = xfrin_finalize_contents(contents_copy, &changes);
	if (ret != KNOT_EOK) {
		xfrin_rollback_update(contents_copy, &changes);
		debug_knot_xfr("Failed to finalize new zone contents: %s\n",
		          knot_strerror(ret));
		return ret;
	}

	/*
	 * Switch the zone contents
	 */
	knot_zone_contents_t *old =
		knot_zone_switch_contents(zone, contents_copy);
	assert(old == old_contents);

	/*
	 * From now on, the new contents of the zone are being used.
	 * References to nodes may be updated in the meantime. However, we must
	 * traverse the zone and fix all references that were not.
	 */
	/*! \todo This operation must not fail!!! .*/
	ret = xfrin_fix_references(contents_copy);
	assert(ret == KNOT_EOK);
	
	// set generation to finished
	knot_zone_contents_set_gen_new_finished(contents_copy);
	
	// set generation of all nodes to the old one
	// now it is safe (no old nodes should be referenced)
	ret = xfrin_fix_generation(contents_copy);
	assert(ret == KNOT_EOK);
	
	/* 
	 * Now we may also set the generation back to 0 so that another 
	 * update is possible.
	 */
	knot_zone_contents_set_gen_old(contents_copy);
	
	/*
	 * Wait until all readers finish reading
	 */
	synchronize_rcu();

	/*
	 * Delete all old and unused data.
	 */
	xfrin_zone_contents_free(&old_contents);
	xfrin_cleanup_update(&changes);

	return KNOT_EOK;
}
