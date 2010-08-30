/*
 * Copyright (c) 2010 Nicira Networks.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "nx-match.h"

#include "classifier.h"
#include "dynamic-string.h"
#include "ofp-util.h"
#include "ofpbuf.h"
#include "openflow/nicira-ext.h"
#include "packets.h"
#include "unaligned.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(nx_match);

/* Rate limit for nx_match parse errors.  These always indicate a bug in the
 * peer and so there's not much point in showing a lot of them. */
static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

enum {
    NXM_INVALID = OFP_MKERR_NICIRA(OFPET_BAD_REQUEST, NXBRC_NXM_INVALID),
    NXM_BAD_TYPE = OFP_MKERR_NICIRA(OFPET_BAD_REQUEST, NXBRC_NXM_BAD_TYPE),
    NXM_BAD_VALUE = OFP_MKERR_NICIRA(OFPET_BAD_REQUEST, NXBRC_NXM_BAD_VALUE),
    NXM_BAD_MASK = OFP_MKERR_NICIRA(OFPET_BAD_REQUEST, NXBRC_NXM_BAD_MASK),
    NXM_BAD_PREREQ = OFP_MKERR_NICIRA(OFPET_BAD_REQUEST, NXBRC_NXM_BAD_PREREQ),
    NXM_DUP_TYPE = OFP_MKERR_NICIRA(OFPET_BAD_REQUEST, NXBRC_NXM_DUP_TYPE),
    BAD_ARGUMENT = OFP_MKERR(OFPET_BAD_ACTION, OFPBAC_BAD_ARGUMENT)
};

/* For each NXM_* field, define NFI_NXM_* as consecutive integers starting from
 * zero. */
enum nxm_field_index {
#define DEFINE_FIELD(HEADER, WILDCARD, DL_TYPE, NW_PROTO) NFI_NXM_##HEADER,
#include "nx-match.def"
    N_NXM_FIELDS
};

struct nxm_field {
    struct hmap_node hmap_node;
    enum nxm_field_index index; /* NFI_* value. */
    uint32_t header;            /* NXM_* value. */
    uint32_t wildcard;          /* Wildcard bit, if exactly one. */
    ovs_be16 dl_type;           /* dl_type prerequisite, if nonzero. */
    uint8_t nw_proto;           /* nw_proto prerequisite, if nonzero. */
    const char *name;           /* "NXM_*" string. */
};

/* All the known fields. */
static struct nxm_field nxm_fields[N_NXM_FIELDS] = {
#define DEFINE_FIELD(HEADER, WILDCARD, DL_TYPE, NW_PROTO) \
    { HMAP_NODE_NULL_INITIALIZER, NFI_NXM_##HEADER, NXM_##HEADER, WILDCARD, \
      CONSTANT_HTONS(DL_TYPE), NW_PROTO, "NXM_" #HEADER },
#include "nx-match.def"
};

/* Hash table of 'nxm_fields'. */
static struct hmap all_nxm_fields = HMAP_INITIALIZER(&all_nxm_fields);

/* Possible masks for NXM_OF_ETH_DST_W. */
static const uint8_t eth_all_0s[ETH_ADDR_LEN]
    = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t eth_all_1s[ETH_ADDR_LEN]
    = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t eth_mcast_1[ETH_ADDR_LEN]
    = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t eth_mcast_0[ETH_ADDR_LEN]
    = {0xfe, 0xff, 0xff, 0xff, 0xff, 0xff};

static void
nxm_init(void)
{
    if (hmap_is_empty(&all_nxm_fields)) {
        int i;

        for (i = 0; i < N_NXM_FIELDS; i++) {
            struct nxm_field *f = &nxm_fields[i];
            hmap_insert(&all_nxm_fields, &f->hmap_node,
                        hash_int(f->header, 0));
        }

        /* Verify that the header values are unique (duplicate "case" values
         * cause a compile error). */
        switch (0) {
#define DEFINE_FIELD(HEADER, WILDCARD, DL_TYPE, NW_PROTO) \
        case NXM_##HEADER: break;
#include "nx-match.def"
        }
    }
}

static const struct nxm_field *
nxm_field_lookup(uint32_t header)
{
    struct nxm_field *f;

    nxm_init();

    HMAP_FOR_EACH_WITH_HASH (f, hmap_node, hash_int(header, 0),
                             &all_nxm_fields) {
        if (f->header == header) {
            return f;
        }
    }

    return NULL;
}

/* Returns the width of the data for a field with the given 'header', in
 * bytes. */
static int
nxm_field_bytes(uint32_t header)
{
    unsigned int length = NXM_LENGTH(header);
    return NXM_HASMASK(header) ? length / 2 : length;
}

/* Returns the width of the data for a field with the given 'header', in
 * bits. */
static int
nxm_field_bits(uint32_t header)
{
    return nxm_field_bytes(header) * 8;
}

/* nx_pull_match() and helpers. */

static int
parse_tci(struct cls_rule *rule, ovs_be16 tci, ovs_be16 mask)
{
    enum { OFPFW_DL_TCI = OFPFW_DL_VLAN | OFPFW_DL_VLAN_PCP };
    if ((rule->wc.wildcards & OFPFW_DL_TCI) != OFPFW_DL_TCI) {
        return NXM_DUP_TYPE;
    } else {
        return cls_rule_set_dl_tci_masked(rule, tci, mask) ? 0 : NXM_INVALID;
    }
}

static int
parse_nx_reg(const struct nxm_field *f,
             struct flow *flow, struct flow_wildcards *wc,
             const void *value, const void *maskp)
{
    int idx = NXM_NX_REG_IDX(f->header);
    if (wc->reg_masks[idx]) {
        return NXM_DUP_TYPE;
    } else {
        flow_wildcards_set_reg_mask(wc, idx,
                                    (NXM_HASMASK(f->header)
                                     ? ntohl(get_unaligned_u32(maskp))
                                     : UINT32_MAX));
        flow->regs[idx] = ntohl(get_unaligned_u32(value));
        flow->regs[idx] &= wc->reg_masks[idx];
        return 0;
    }
}

static int
parse_nxm_entry(struct cls_rule *rule, const struct nxm_field *f,
                const void *value, const void *mask)
{
    struct flow_wildcards *wc = &rule->wc;
    struct flow *flow = &rule->flow;

    switch (f->index) {
        /* Metadata. */
    case NFI_NXM_OF_IN_PORT:
        flow->in_port = ntohs(get_unaligned_u16(value));
        if (flow->in_port == OFPP_LOCAL) {
            flow->in_port = ODPP_LOCAL;
        }
        return 0;

        /* Ethernet header. */
    case NFI_NXM_OF_ETH_DST:
        if ((wc->wildcards & (OFPFW_DL_DST | FWW_ETH_MCAST))
            != (OFPFW_DL_DST | FWW_ETH_MCAST)) {
            return NXM_DUP_TYPE;
        } else {
            wc->wildcards &= ~(OFPFW_DL_DST | FWW_ETH_MCAST);
            memcpy(flow->dl_dst, value, ETH_ADDR_LEN);
            return 0;
        }
    case NFI_NXM_OF_ETH_DST_W:
        if ((wc->wildcards & (OFPFW_DL_DST | FWW_ETH_MCAST))
            != (OFPFW_DL_DST | FWW_ETH_MCAST)) {
            return NXM_DUP_TYPE;
        } else if (eth_addr_equals(mask, eth_mcast_1)) {
            wc->wildcards &= ~FWW_ETH_MCAST;
            flow->dl_dst[0] = *(uint8_t *) value & 0x01;
        } else if (eth_addr_equals(mask, eth_mcast_0)) {
            wc->wildcards &= ~OFPFW_DL_DST;
            memcpy(flow->dl_dst, value, ETH_ADDR_LEN);
            flow->dl_dst[0] &= 0xfe;
        } else if (eth_addr_equals(mask, eth_all_0s)) {
            return 0;
        } else if (eth_addr_equals(mask, eth_all_1s)) {
            wc->wildcards &= ~(OFPFW_DL_DST | FWW_ETH_MCAST);
            memcpy(flow->dl_dst, value, ETH_ADDR_LEN);
            return 0;
        } else {
            return NXM_BAD_MASK;
        }
    case NFI_NXM_OF_ETH_SRC:
        memcpy(flow->dl_src, value, ETH_ADDR_LEN);
        return 0;
    case NFI_NXM_OF_ETH_TYPE:
        flow->dl_type = get_unaligned_u16(value);
        return 0;

        /* 802.1Q header. */
    case NFI_NXM_OF_VLAN_TCI:
        return parse_tci(rule, get_unaligned_u16(value), htons(UINT16_MAX));

    case NFI_NXM_OF_VLAN_TCI_W:
        return parse_tci(rule, get_unaligned_u16(value),
                         get_unaligned_u16(mask));

        /* IP header. */
    case NFI_NXM_OF_IP_TOS:
        if (*(uint8_t *) value & 0x03) {
            return NXM_BAD_VALUE;
        } else {
            flow->nw_tos = *(uint8_t *) value;
            return 0;
        }
    case NFI_NXM_OF_IP_PROTO:
        flow->nw_proto = *(uint8_t *) value;
        return 0;

        /* IP addresses in IP and ARP headers. */
    case NFI_NXM_OF_IP_SRC:
    case NFI_NXM_OF_ARP_SPA:
        if (wc->nw_src_mask) {
            return NXM_DUP_TYPE;
        } else {
            cls_rule_set_nw_src(rule, get_unaligned_u32(value));
            return 0;
        }
    case NFI_NXM_OF_IP_SRC_W:
    case NFI_NXM_OF_ARP_SPA_W:
        if (wc->nw_src_mask) {
            return NXM_DUP_TYPE;
        } else {
            ovs_be32 ip = get_unaligned_u32(value);
            ovs_be32 netmask = get_unaligned_u32(mask);
            if (!cls_rule_set_nw_src_masked(rule, ip, netmask)) {
                return NXM_BAD_MASK;
            }
            return 0;
        }
    case NFI_NXM_OF_IP_DST:
    case NFI_NXM_OF_ARP_TPA:
        if (wc->nw_dst_mask) {
            return NXM_DUP_TYPE;
        } else {
            cls_rule_set_nw_dst(rule, get_unaligned_u32(value));
            return 0;
        }
    case NFI_NXM_OF_IP_DST_W:
    case NFI_NXM_OF_ARP_TPA_W:
        if (wc->nw_dst_mask) {
            return NXM_DUP_TYPE;
        } else {
            ovs_be32 ip = get_unaligned_u32(value);
            ovs_be32 netmask = get_unaligned_u32(mask);
            if (!cls_rule_set_nw_dst_masked(rule, ip, netmask)) {
                return NXM_BAD_MASK;
            }
            return 0;
        }

        /* TCP header. */
    case NFI_NXM_OF_TCP_SRC:
        flow->tp_src = get_unaligned_u16(value);
        return 0;
    case NFI_NXM_OF_TCP_DST:
        flow->tp_dst = get_unaligned_u16(value);
        return 0;

        /* UDP header. */
    case NFI_NXM_OF_UDP_SRC:
        flow->tp_src = get_unaligned_u16(value);
        return 0;
    case NFI_NXM_OF_UDP_DST:
        flow->tp_dst = get_unaligned_u16(value);
        return 0;

        /* ICMP header. */
    case NFI_NXM_OF_ICMP_TYPE:
        flow->tp_src = htons(*(uint8_t *) value);
        return 0;
    case NFI_NXM_OF_ICMP_CODE:
        flow->tp_dst = htons(*(uint8_t *) value);
        return 0;

        /* ARP header. */
    case NFI_NXM_OF_ARP_OP:
        if (ntohs(get_unaligned_u16(value)) > 255) {
            return NXM_BAD_VALUE;
        } else {
            flow->nw_proto = ntohs(get_unaligned_u16(value));
            return 0;
        }

        /* Tunnel ID. */
    case NFI_NXM_NX_TUN_ID:
        flow->tun_id = htonl(ntohll(get_unaligned_u64(value)));
        return 0;

        /* Registers. */
    case NFI_NXM_NX_REG0:
    case NFI_NXM_NX_REG0_W:
#if FLOW_N_REGS >= 2
    case NFI_NXM_NX_REG1:
    case NFI_NXM_NX_REG1_W:
#endif
#if FLOW_N_REGS >= 3
    case NFI_NXM_NX_REG2:
    case NFI_NXM_NX_REG2_W:
#endif
#if FLOW_N_REGS >= 4
    case NFI_NXM_NX_REG3:
    case NFI_NXM_NX_REG3_W:
#endif
#if FLOW_N_REGS > 4
#error
#endif
        return parse_nx_reg(f, flow, wc, value, mask);

    case N_NXM_FIELDS:
        NOT_REACHED();
    }
    NOT_REACHED();
}

static bool
nxm_prereqs_ok(const struct nxm_field *field, const struct flow *flow)
{
    return (!field->dl_type
            || (field->dl_type == flow->dl_type
                && (!field->nw_proto || field->nw_proto == flow->nw_proto)));
}

static uint32_t
nx_entry_ok(const void *p, unsigned int match_len)
{
    unsigned int payload_len;
    ovs_be32 header_be;
    uint32_t header;

    if (match_len < 4) {
        if (match_len) {
            VLOG_DBG_RL(&rl, "nx_match ends with partial nxm_header");
        }
        return 0;
    }
    memcpy(&header_be, p, 4);
    header = ntohl(header_be);

    payload_len = NXM_LENGTH(header);
    if (!payload_len) {
        VLOG_DBG_RL(&rl, "nxm_entry %08"PRIx32" has invalid payload "
                    "length 0", header);
        return 0;
    }
    if (match_len < payload_len + 4) {
        VLOG_DBG_RL(&rl, "%"PRIu32"-byte nxm_entry but only "
                    "%u bytes left in nx_match", payload_len + 4, match_len);
        return 0;
    }

    return header;
}

int
nx_pull_match(struct ofpbuf *b, unsigned int match_len, uint16_t priority,
              struct cls_rule *rule)
{
    uint32_t header;
    uint8_t *p;

    p = ofpbuf_try_pull(b, ROUND_UP(match_len, 8));
    if (!p) {
        VLOG_DBG_RL(&rl, "nx_match length %zu, rounded up to a "
                    "multiple of 8, is longer than space in message (max "
                    "length %zu)", match_len, b->size);
        return ofp_mkerr(OFPET_BAD_REQUEST, OFPBRC_BAD_LEN);
    }

    cls_rule_init_catchall(rule, priority);
    while ((header = nx_entry_ok(p, match_len)) != 0) {
        unsigned length = NXM_LENGTH(header);
        const struct nxm_field *f;
        int error;

        f = nxm_field_lookup(header);
        if (!f) {
            error = NXM_BAD_TYPE;
        } else if (!nxm_prereqs_ok(f, &rule->flow)) {
            error = NXM_BAD_PREREQ;
        } else if (f->wildcard && !(rule->wc.wildcards & f->wildcard)) {
            error = NXM_DUP_TYPE;
        } else {
            /* 'hasmask' and 'length' are known to be correct at this point
             * because they are included in 'header' and nxm_field_lookup()
             * checked them already. */
            rule->wc.wildcards &= ~f->wildcard;
            error = parse_nxm_entry(rule, f, p + 4, p + 4 + length / 2);
        }
        if (error) {
            VLOG_DBG_RL(&rl, "bad nxm_entry with vendor=%"PRIu32", "
                        "field=%"PRIu32", hasmask=%"PRIu32", type=%"PRIu32" "
                        "(error %x)",
                        NXM_VENDOR(header), NXM_FIELD(header),
                        NXM_HASMASK(header), NXM_TYPE(header),
                        error);
            return error;
        }


        p += 4 + length;
        match_len -= 4 + length;
    }

    return match_len ? NXM_INVALID : 0;
}

/* nx_put_match() and helpers.
 *
 * 'put' functions whose names end in 'w' add a wildcarded field.
 * 'put' functions whose names end in 'm' add a field that might be wildcarded.
 * Other 'put' functions add exact-match fields.
 */

static void
nxm_put_header(struct ofpbuf *b, uint32_t header)
{
    ovs_be32 n_header = htonl(header);
    ofpbuf_put(b, &n_header, sizeof n_header);
}

static void
nxm_put_8(struct ofpbuf *b, uint32_t header, uint8_t value)
{
    nxm_put_header(b, header);
    ofpbuf_put(b, &value, sizeof value);
}

static void
nxm_put_16(struct ofpbuf *b, uint32_t header, ovs_be16 value)
{
    nxm_put_header(b, header);
    ofpbuf_put(b, &value, sizeof value);
}

static void
nxm_put_16w(struct ofpbuf *b, uint32_t header, ovs_be16 value, ovs_be16 mask)
{
    nxm_put_header(b, header);
    ofpbuf_put(b, &value, sizeof value);
    ofpbuf_put(b, &mask, sizeof mask);
}

static void
nxm_put_32(struct ofpbuf *b, uint32_t header, ovs_be32 value)
{
    nxm_put_header(b, header);
    ofpbuf_put(b, &value, sizeof value);
}

static void
nxm_put_32w(struct ofpbuf *b, uint32_t header, ovs_be32 value, ovs_be32 mask)
{
    nxm_put_header(b, header);
    ofpbuf_put(b, &value, sizeof value);
    ofpbuf_put(b, &mask, sizeof mask);
}

static void
nxm_put_32m(struct ofpbuf *b, uint32_t header, ovs_be32 value, ovs_be32 mask)
{
    switch (mask) {
    case 0:
        break;

    case UINT32_MAX:
        nxm_put_32(b, header, value);
        break;

    default:
        nxm_put_32w(b, NXM_MAKE_WILD_HEADER(header), value, mask);
        break;
    }
}

static void
nxm_put_64(struct ofpbuf *b, uint32_t header, ovs_be64 value)
{
    nxm_put_header(b, header);
    ofpbuf_put(b, &value, sizeof value);
}

static void
nxm_put_eth(struct ofpbuf *b, uint32_t header,
            const uint8_t value[ETH_ADDR_LEN])
{
    nxm_put_header(b, header);
    ofpbuf_put(b, value, ETH_ADDR_LEN);
}

static void
nxm_put_eth_dst(struct ofpbuf *b,
                uint32_t wc, const uint8_t value[ETH_ADDR_LEN])
{
    switch (wc & (OFPFW_DL_DST | FWW_ETH_MCAST)) {
    case OFPFW_DL_DST | FWW_ETH_MCAST:
        break;
    case OFPFW_DL_DST:
        nxm_put_header(b, NXM_OF_ETH_DST_W);
        ofpbuf_put(b, value, ETH_ADDR_LEN);
        ofpbuf_put(b, eth_mcast_1, ETH_ADDR_LEN);
        break;
    case FWW_ETH_MCAST:
        nxm_put_header(b, NXM_OF_ETH_DST_W);
        ofpbuf_put(b, value, ETH_ADDR_LEN);
        ofpbuf_put(b, eth_mcast_0, ETH_ADDR_LEN);
        break;
    case 0:
        nxm_put_eth(b, NXM_OF_ETH_DST, value);
        break;
    }
}

int
nx_put_match(struct ofpbuf *b, const struct cls_rule *cr)
{
    const uint32_t wc = cr->wc.wildcards;
    const struct flow *flow = &cr->flow;
    const size_t start_len = b->size;
    ovs_be16 vid, pcp;
    int match_len;
    int i;

    /* Metadata. */
    if (!(wc & OFPFW_IN_PORT)) {
        uint16_t in_port = flow->in_port;
        if (in_port == ODPP_LOCAL) {
            in_port = OFPP_LOCAL;
        }
        nxm_put_16(b, NXM_OF_IN_PORT, htons(in_port));
    }

    /* Ethernet. */
    nxm_put_eth_dst(b, wc, flow->dl_dst);
    if (!(wc & OFPFW_DL_SRC)) {
        nxm_put_eth(b, NXM_OF_ETH_SRC, flow->dl_src);
    }
    if (!(wc & OFPFW_DL_TYPE)) {
        nxm_put_16(b, NXM_OF_ETH_TYPE, flow->dl_type);
    }

    /* 802.1Q. */
    vid = flow->dl_vlan & htons(VLAN_VID_MASK);
    pcp = htons((flow->dl_vlan_pcp << VLAN_PCP_SHIFT) & VLAN_PCP_MASK);
    switch (wc & (OFPFW_DL_VLAN | OFPFW_DL_VLAN_PCP)) {
    case OFPFW_DL_VLAN | OFPFW_DL_VLAN_PCP:
        break;
    case OFPFW_DL_VLAN:
        nxm_put_16w(b, NXM_OF_VLAN_TCI_W, pcp | htons(VLAN_CFI),
                     htons(VLAN_PCP_MASK | VLAN_CFI));
        break;
    case OFPFW_DL_VLAN_PCP:
        if (flow->dl_vlan == htons(OFP_VLAN_NONE)) {
            nxm_put_16(b, NXM_OF_VLAN_TCI, 0);
        } else {
            nxm_put_16w(b, NXM_OF_VLAN_TCI_W, vid | htons(VLAN_CFI),
                         htons(VLAN_VID_MASK | VLAN_CFI));
        }
        break;
    case 0:
        if (flow->dl_vlan == htons(OFP_VLAN_NONE)) {
            nxm_put_16(b, NXM_OF_VLAN_TCI, 0);
        } else {
            nxm_put_16(b, NXM_OF_VLAN_TCI, vid | pcp | htons(VLAN_CFI));
        }
        break;
    }

    if (!(wc & OFPFW_DL_TYPE) && flow->dl_type == htons(ETH_TYPE_IP)) {
        /* IP. */
        if (!(wc & OFPFW_NW_TOS)) {
            nxm_put_8(b, NXM_OF_IP_TOS, flow->nw_tos & 0xfc);
        }
        nxm_put_32m(b, NXM_OF_IP_SRC, flow->nw_src, cr->wc.nw_src_mask);
        nxm_put_32m(b, NXM_OF_IP_DST, flow->nw_dst, cr->wc.nw_dst_mask);

        if (!(wc & OFPFW_NW_PROTO)) {
            nxm_put_8(b, NXM_OF_IP_PROTO, flow->nw_proto);
            switch (flow->nw_proto) {
                /* TCP. */
            case IP_TYPE_TCP:
                if (!(wc & OFPFW_TP_SRC)) {
                    nxm_put_16(b, NXM_OF_TCP_SRC, flow->tp_src);
                }
                if (!(wc & OFPFW_TP_DST)) {
                    nxm_put_16(b, NXM_OF_TCP_DST, flow->tp_dst);
                }
                break;

                /* UDP. */
            case IP_TYPE_UDP:
                if (!(wc & OFPFW_TP_SRC)) {
                    nxm_put_16(b, NXM_OF_UDP_SRC, flow->tp_src);
                }
                if (!(wc & OFPFW_TP_DST)) {
                    nxm_put_16(b, NXM_OF_UDP_DST, flow->tp_dst);
                }
                break;

                /* ICMP. */
            case IP_TYPE_ICMP:
                if (!(wc & OFPFW_TP_SRC)) {
                    nxm_put_8(b, NXM_OF_ICMP_TYPE, ntohs(flow->tp_src));
                }
                if (!(wc & OFPFW_TP_DST)) {
                    nxm_put_8(b, NXM_OF_ICMP_CODE, ntohs(flow->tp_dst));
                }
                break;
            }
        }
    } else if (!(wc & OFPFW_DL_TYPE) && flow->dl_type == htons(ETH_TYPE_ARP)) {
        /* ARP. */
        if (!(wc & OFPFW_NW_PROTO)) {
            nxm_put_16(b, NXM_OF_ARP_OP, htons(flow->nw_proto));
        }
        nxm_put_32m(b, NXM_OF_ARP_SPA, flow->nw_src, cr->wc.nw_src_mask);
        nxm_put_32m(b, NXM_OF_ARP_TPA, flow->nw_dst, cr->wc.nw_dst_mask);
    }

    /* Tunnel ID. */
    if (!(wc & NXFW_TUN_ID)) {
        nxm_put_64(b, NXM_NX_TUN_ID, htonll(ntohl(flow->tun_id)));
    }

    /* Registers. */
    for (i = 0; i < FLOW_N_REGS; i++) {
        nxm_put_32m(b, NXM_NX_REG(i),
                    htonl(flow->regs[i]), htonl(cr->wc.reg_masks[i]));
    }

    match_len = b->size - start_len;
    ofpbuf_put_zeros(b, ROUND_UP(match_len, 8) - match_len);
    return match_len;
}

/* nx_match_to_string() and helpers. */

char *
nx_match_to_string(const uint8_t *p, unsigned int match_len)
{
    uint32_t header;
    struct ds s;

    if (!match_len) {
        return xstrdup("<any>");
    }

    ds_init(&s);
    while ((header = nx_entry_ok(p, match_len)) != 0) {
        unsigned int length = NXM_LENGTH(header);
        unsigned int value_len = nxm_field_bytes(header);
        const uint8_t *value = p + 4;
        const uint8_t *mask = value + value_len;
        const struct nxm_field *f;
        unsigned int i;

        if (s.length) {
            ds_put_cstr(&s, ", ");
        }

        f = nxm_field_lookup(header);
        if (f) {
            ds_put_cstr(&s, f->name);
        } else {
            ds_put_format(&s, "%d:%d", NXM_VENDOR(header), NXM_FIELD(header));
        }

        ds_put_char(&s, '(');

        for (i = 0; i < value_len; i++) {
            ds_put_format(&s, "%02x", value[i]);
        }
        if (NXM_HASMASK(header)) {
            ds_put_char(&s, '/');
            for (i = 0; i < value_len; i++) {
                ds_put_format(&s, "%02x", mask[i]);
            }
        }
        ds_put_char(&s, ')');

        p += 4 + length;
        match_len -= 4 + length;
    }

    if (match_len) {
        if (s.length) {
            ds_put_cstr(&s, ", ");
        }

        ds_put_format(&s, "<%u invalid bytes>", match_len);
    }

    return ds_steal_cstr(&s);
}

static const struct nxm_field *
lookup_nxm_field(const char *name, int name_len)
{
    const struct nxm_field *f;

    for (f = nxm_fields; f < &nxm_fields[ARRAY_SIZE(nxm_fields)]; f++) {
        if (!strncmp(f->name, name, name_len) && f->name[name_len] == '\0') {
            return f;
        }
    }

    return NULL;
}

static const char *
parse_hex_bytes(struct ofpbuf *b, const char *s, unsigned int n)
{
    while (n--) {
        uint8_t byte;
        bool ok;

        s += strspn(s, " ");
        byte = hexits_value(s, 2, &ok);
        if (!ok) {
            ovs_fatal(0, "%.2s: hex digits expected", s);
        }

        ofpbuf_put(b, &byte, 1);
        s += 2;
    }
    return s;
}

/* nx_match_from_string(). */

int
nx_match_from_string(const char *s, struct ofpbuf *b)
{
    const char *full_s = s;
    const size_t start_len = b->size;
    int match_len;

    if (!strcmp(s, "<any>")) {
        /* Ensure that 'b->data' isn't actually null. */
        ofpbuf_prealloc_tailroom(b, 1);
        return 0;
    }

    for (s += strspn(s, ", "); *s; s += strspn(s, ", ")) {
        const struct nxm_field *f;
        int name_len;

        name_len = strcspn(s, "(");
        if (s[name_len] != '(') {
            ovs_fatal(0, "%s: missing ( at end of nx_match", full_s);
        }

        f = lookup_nxm_field(s, name_len);
        if (!f) {
            ovs_fatal(0, "%s: unknown field `%.*s'", full_s, name_len, s);
        }

        s += name_len + 1;

        nxm_put_header(b, f->header);
        s = parse_hex_bytes(b, s, nxm_field_bytes(f->header));
        if (NXM_HASMASK(f->header)) {
            s += strspn(s, " ");
            if (*s != '/') {
                ovs_fatal(0, "%s: missing / in masked field %s",
                          full_s, f->name);
            }
            s = parse_hex_bytes(b, s + 1, nxm_field_bytes(f->header));
        }

        s += strspn(s, " ");
        if (*s != ')') {
            ovs_fatal(0, "%s: missing ) following field %s", full_s, f->name);
        }
        s++;
    }

    match_len = b->size - start_len;
    ofpbuf_put_zeros(b, ROUND_UP(match_len, 8) - match_len);
    return match_len;
}

/* nxm_check_reg_move(), nxm_check_reg_load(). */

static bool
field_ok(const struct nxm_field *f, const struct flow *flow, int size)
{
    return (f && !NXM_HASMASK(f->header)
            && nxm_prereqs_ok(f, flow) && size <= nxm_field_bits(f->header));
}

int
nxm_check_reg_move(const struct nx_action_reg_move *action,
                   const struct flow *flow)
{
    const struct nxm_field *src;
    const struct nxm_field *dst;

    if (action->n_bits == htons(0)) {
        return BAD_ARGUMENT;
    }

    src = nxm_field_lookup(ntohl(action->src));
    if (!field_ok(src, flow, ntohs(action->src_ofs) + ntohs(action->n_bits))) {
        return BAD_ARGUMENT;
    }

    dst = nxm_field_lookup(ntohl(action->dst));
    if (!field_ok(dst, flow, ntohs(action->dst_ofs) + ntohs(action->n_bits))) {
        return BAD_ARGUMENT;
    }

    if (!NXM_IS_NX_REG(dst->header)
        && dst->header != NXM_OF_VLAN_TCI
        && dst->header != NXM_NX_TUN_ID) {
        return BAD_ARGUMENT;
    }

    return 0;
}

int
nxm_check_reg_load(const struct nx_action_reg_load *action,
                   const struct flow *flow)
{
    const struct nxm_field *dst;
    int ofs, n_bits;

    ofs = ntohs(action->ofs_nbits) >> 6;
    n_bits = (ntohs(action->ofs_nbits) & 0x3f) + 1;
    dst = nxm_field_lookup(ntohl(action->dst));
    if (!field_ok(dst, flow, ofs + n_bits)) {
        return BAD_ARGUMENT;
    }

    /* Reject 'action' if a bit numbered 'n_bits' or higher is set to 1 in
     * action->value. */
    if (n_bits < 64 && ntohll(action->value) >> n_bits) {
        return BAD_ARGUMENT;
    }

    if (!NXM_IS_NX_REG(dst->header)) {
        return BAD_ARGUMENT;
    }

    return 0;
}

/* nxm_execute_reg_move(), nxm_execute_reg_load(). */

static uint64_t
nxm_read_field(const struct nxm_field *src, const struct flow *flow)
{
    switch (src->index) {
    case NFI_NXM_OF_IN_PORT:
        return flow->in_port == ODPP_LOCAL ? OFPP_LOCAL : flow->in_port;

    case NFI_NXM_OF_ETH_DST:
        return eth_addr_to_uint64(flow->dl_dst);

    case NFI_NXM_OF_ETH_SRC:
        return eth_addr_to_uint64(flow->dl_src);

    case NFI_NXM_OF_ETH_TYPE:
        return ntohs(flow->dl_type);

    case NFI_NXM_OF_VLAN_TCI:
        if (flow->dl_vlan == htons(OFP_VLAN_NONE)) {
            return 0;
        } else {
            return (ntohs(flow->dl_vlan & htons(VLAN_VID_MASK))
                    | ((flow->dl_vlan_pcp << VLAN_PCP_SHIFT) & VLAN_PCP_MASK)
                    | VLAN_CFI);
        }

    case NFI_NXM_OF_IP_TOS:
        return flow->nw_tos;

    case NFI_NXM_OF_IP_PROTO:
    case NFI_NXM_OF_ARP_OP:
        return flow->nw_proto;

    case NFI_NXM_OF_IP_SRC:
    case NFI_NXM_OF_ARP_SPA:
        return ntohl(flow->nw_src);

    case NFI_NXM_OF_IP_DST:
    case NFI_NXM_OF_ARP_TPA:
        return ntohl(flow->nw_dst);

    case NFI_NXM_OF_TCP_SRC:
    case NFI_NXM_OF_UDP_SRC:
        return ntohs(flow->tp_src);

    case NFI_NXM_OF_TCP_DST:
    case NFI_NXM_OF_UDP_DST:
        return ntohs(flow->tp_dst);

    case NFI_NXM_OF_ICMP_TYPE:
        return ntohs(flow->tp_src) & 0xff;

    case NFI_NXM_OF_ICMP_CODE:
        return ntohs(flow->tp_dst) & 0xff;

    case NFI_NXM_NX_TUN_ID:
        return ntohl(flow->tun_id);

#define NXM_READ_REGISTER(IDX)                  \
    case NFI_NXM_NX_REG##IDX:                   \
        return flow->regs[IDX];                 \
    case NFI_NXM_NX_REG##IDX##_W:               \
        NOT_REACHED();

    NXM_READ_REGISTER(0);
#if FLOW_N_REGS >= 2
    NXM_READ_REGISTER(1);
#endif
#if FLOW_N_REGS >= 3
    NXM_READ_REGISTER(2);
#endif
#if FLOW_N_REGS >= 4
    NXM_READ_REGISTER(3);
#endif
#if FLOW_N_REGS > 4
#error
#endif

    case NFI_NXM_OF_ETH_DST_W:
    case NFI_NXM_OF_VLAN_TCI_W:
    case NFI_NXM_OF_IP_SRC_W:
    case NFI_NXM_OF_IP_DST_W:
    case NFI_NXM_OF_ARP_SPA_W:
    case NFI_NXM_OF_ARP_TPA_W:
    case N_NXM_FIELDS:
        NOT_REACHED();
    }

    NOT_REACHED();
}

void
nxm_execute_reg_move(const struct nx_action_reg_move *action,
                     struct flow *flow)
{
    /* Preparation. */
    int n_bits = ntohs(action->n_bits);
    uint64_t mask = n_bits == 64 ? UINT64_MAX : (UINT64_C(1) << n_bits) - 1;

    /* Get the interesting bits of the source field. */
    const struct nxm_field *src = nxm_field_lookup(ntohl(action->src));
    int src_ofs = ntohs(action->src_ofs);
    uint64_t src_data = nxm_read_field(src, flow) & (mask << src_ofs);

    /* Get the remaining bits of the destination field. */
    const struct nxm_field *dst = nxm_field_lookup(ntohl(action->dst));
    int dst_ofs = ntohs(action->dst_ofs);
    uint64_t dst_data = nxm_read_field(dst, flow) & ~(mask << dst_ofs);

    /* Get the final value. */
    uint64_t new_data = dst_data | ((src_data >> src_ofs) << dst_ofs);

    /* Store the result. */
    if (NXM_IS_NX_REG(dst->header)) {
        flow->regs[NXM_NX_REG_IDX(dst->header)] = new_data;
    } else if (dst->header == NXM_OF_VLAN_TCI) {
        ovs_be16 vlan_tci = htons(new_data & VLAN_CFI ? new_data : 0);
        flow->dl_vlan = htons(vlan_tci_to_vid(vlan_tci));
        flow->dl_vlan_pcp = vlan_tci_to_pcp(vlan_tci);
    } else if (dst->header == NXM_NX_TUN_ID) {
        flow->tun_id = htonl(new_data);
    } else {
        NOT_REACHED();
    }
}

void
nxm_execute_reg_load(const struct nx_action_reg_load *action,
                     struct flow *flow)
{
    /* Preparation. */
    int n_bits = (ntohs(action->ofs_nbits) & 0x3f) + 1;
    uint32_t mask = n_bits == 32 ? UINT32_MAX : (UINT32_C(1) << n_bits) - 1;
    uint32_t *reg = &flow->regs[NXM_NX_REG_IDX(ntohl(action->dst))];

    /* Get source data. */
    uint32_t src_data = ntohll(action->value);

    /* Get remaining bits of the destination field. */
    int dst_ofs = ntohs(action->ofs_nbits) >> 6;
    uint32_t dst_data = *reg & ~(mask << dst_ofs);

    *reg = dst_data | (src_data << dst_ofs);
}