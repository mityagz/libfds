/**
 * \file   src/template_mgr/template.c
 * \author Lukas Hutak <lukas.hutak@cesnet.cz>
 * \brief  Template (source file)
 * \date   October 2017
 */
/*
 * Copyright (C) 2017 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#include <stddef.h>    // size_t
#include <arpa/inet.h> // ntohs
#include <string.h>    // memcpy
#include <strings.h>   // strcasecmp
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#include <libfds/template.h>
#include "../ipfix_structures.h"

/**
 * Calculate size of template structure (based on number of fields)
 */
#define TEMPLATE_STRUCT_SIZE(elem_cnt) \
    sizeof(struct fds_template) \
    - sizeof(struct fds_tfield) \
    + ((elem_cnt) * sizeof(struct fds_tfield))

/** Return only first bit from a _value_ */
#define EN_BIT_GET(_value_)  ((_value_) & (uint16_t) 0x8000)
/** Return _value_ without the first bit */
#define EN_BIT_MASK(_value_) ((_value_) & (uint16_t )0x7FFF)
/** Get the length of an array */
#define ARRAY_SIZE(_arr_) (sizeof(_arr_) / sizeof((_arr_)[0]))

/** Required field identification            */
struct opts_req_id {
    uint16_t id; /**< Information Element ID */
    uint32_t en; /**< Enterprise Number      */
};

/**
 * \brief Check presence of required non-scope Information Elements (IEs)
 * \note All scope IEs are ignored!
 * \param[in] tmplt   Template structure
 * \param[in] recs    Array of required Information Elements
 * \param[in] rec_cnt Number of elements in the array
 * \return If all required IEs are present, the function will return true. Otherwise, returns false.
 */
static bool
opts_has_required(const struct fds_template *tmplt, const struct opts_req_id *recs,
    size_t rec_cnt)
{
    const uint16_t idx_start = tmplt->fields_cnt_scope;
    const uint16_t idx_end = tmplt->fields_cnt_total;

    // Try to find each required field
    for (size_t rec_idx = 0; rec_idx < rec_cnt; ++rec_idx) {
        const struct opts_req_id rec = recs[rec_idx];
        const struct fds_tfield *field_ptr = NULL;
        uint16_t field_idx;

        for (field_idx = idx_start; field_idx < idx_end; ++field_idx) {
            field_ptr = &tmplt->fields[field_idx];
            if (rec.id != field_ptr->id || rec.en != field_ptr->en) {
                continue;
            }

            break;
        }

        if (field_idx == idx_end) {
            // Required field not found!
            return false;
        }
    }

    return true;
}

/**
 * \brief Check presence of non-scope observation time interval
 *
 * The function will try to find any 2 "observationTimeXXX" Information Elements, where XXX is
 * one of following: Seconds, Milliseconds, Microseconds, Nanoseconds.
 * \note All scope IEs are ignored!
 * \param[in] tmplt Template structure
 * \return If the interval is present, returns true. Otherwise, returns false.
 */
static bool
opts_has_obs_time(const struct fds_template *tmplt)
{
    unsigned int matches = 0;

    const uint16_t fields_start = tmplt->fields_cnt_scope;
    const uint16_t fields_end = tmplt->fields_cnt_total;
    for (uint16_t i = fields_start; i < fields_end; ++i) {
        const struct fds_tfield *field_ptr = &tmplt->fields[i];
        if (field_ptr->en != 0) {
            continue;
        }

        /* We are looking for IEs observationTimeXXX with different precision
         * observationTimeSeconds (322) - observationTimeNanoseconds (325)
         */
        if (field_ptr->id < 322 || field_ptr->id > 325) {
            continue;
        }

        matches++;
        if (matches > 2) {
            // Too many matches
            return false;
        }
    }
    return (matches == 2);
}

/**
 * \brief Detect Options Template types of Metering Process
 *
 * If one or more types are detected, appropriate flag(s) will be set.
 * \note Based on RFC 7011, Section 4.1. - 4.2..
 * \param[in] tmplt Template structure
 */
static void
opts_detect_mproc(struct fds_template *tmplt)
{
    // Metering Process Template
    const uint16_t IPFIX_IE_ODID = 149; // observationDomainId
    const uint16_t IPFIX_IE_MPID = 143; // meteringProcessId
    const struct fds_tfield *odid_ptr = fds_template_find(tmplt, 0, IPFIX_IE_ODID);
    const struct fds_tfield *mpid_ptr = fds_template_find(tmplt, 0, IPFIX_IE_MPID);
    if (odid_ptr == NULL && mpid_ptr == NULL) {
        // At least one field must be defined
        return;
    }

    // Check scope fields
    const struct fds_tfield *ptrs[] = {odid_ptr, mpid_ptr};
    for (size_t i = 0; i < ARRAY_SIZE(ptrs); ++i) {
        const struct fds_tfield *ptr = ptrs[i];
        if (ptr == NULL) {
            // Item not found, skip
            continue;
        }

        if ((ptr->flags & FDS_TFIELD_SCOPE) == 0) {
            // The field found, but not in the scope!
            return;
        }

        if (ptr->flags & FDS_TFIELD_MULTI_IE) {
            // Multiple definitions are not expected!
            return;
        }
    }

    // Check non-scope fields
    static const struct opts_req_id ids_mproc[] = {
        {40, 0}, // exportedOctetTotalCount
        {41, 0}, // exportedMessageTotalCount
        {42, 0}  // exportedFlowRecordTotalCount
    };

    if (opts_has_required(tmplt, ids_mproc, ARRAY_SIZE(ids_mproc))) {
        // Ok, this is definitely "The Metering Process Statistics Options Template"
        tmplt->opts_types |= FDS_OPTS_MPROC_STAT;
    }

    static const struct opts_req_id ids_mproc_stat[] = {
        {164, 0}, // ignoredPacketTotalCount
        {165, 0}  // ignoredOctetTotalCount
    };
    if (!opts_has_required(tmplt, ids_mproc_stat, ARRAY_SIZE(ids_mproc_stat))) {
        // Required fields not found
        return;
    }

    if (opts_has_obs_time(tmplt)) {
        // Ok, this is definitely "The Metering Process Reliability Statistics Options Template"
        tmplt->opts_types |= FDS_OPTS_MPROC_RELIABILITY_STAT;
    }
}

/**
 * \brief Detect Options Template type of Exporting Process
 *
 * If the type is detected, an appropriate flag will be set.
 * \note Based on RFC 7011, Section 4.3.
 * \param[in] tmplt Template structure
 */
static void
opts_detect_eproc(struct fds_template *tmplt)
{
    const uint16_t IPFIX_IE_EXP_IPV4 = 130; // exporterIPv4Address
    const uint16_t IPFIX_IE_EXP_IPV6 = 131; // exporterIPv6Address
    const uint16_t IPFIX_IE_EXP_PID = 144;  // exportingProcessId

    // Check scope fields
    bool eid_found = false;
    const uint16_t eid[] = {IPFIX_IE_EXP_IPV4, IPFIX_IE_EXP_IPV6, IPFIX_IE_EXP_PID};
    for (size_t i = 0; i < ARRAY_SIZE(eid); ++i) {
        const struct fds_tfield *field_ptr = fds_template_find(tmplt, 0, eid[i]);
        if (!field_ptr) {
            // Not found
            continue;
        }

        if (field_ptr->flags & FDS_TFIELD_SCOPE && field_ptr->flags & FDS_TFIELD_LAST_IE) {
            eid_found = true;
            break;
        }
    }

    if (!eid_found) {
        return;
    }

    // Check non-scope fields
    static const struct opts_req_id ids_exp[] = {
        {166, 0}, // notSentFlowTotalCount
        {167, 0}, // notSentPacketTotalCount
        {168, 0}  // notSentOctetTotalCount
    };
    if (!opts_has_required(tmplt, ids_exp, ARRAY_SIZE(ids_exp))) {
        // Required fields not found
        return;
    }

    if (opts_has_obs_time(tmplt)) {
        // Ok, this is definitely "The Exporting Process Reliability Statistics Options Template"
        tmplt->opts_types |= FDS_OPTS_EPROC_RELIABILITY_STAT;
    }
}

/**
 * \brief Detect Options Template type of Flow keys
 *
 * If the type is detected, an appropriate flag will be set.
 * \note Based on RFC 7011, Section 4.4.
 * \param[in] tmplt Template structure
 */
static void
opts_detect_flowkey(struct fds_template *tmptl)
{
    // Check scope Field
    const uint16_t IPFIX_IE_TEMPLATE_ID = 145;
    const struct fds_tfield *id_ptr = fds_template_find(tmptl, 0, IPFIX_IE_TEMPLATE_ID);
    if (id_ptr == NULL) {
        // Not found
        return;
    }

    if ((id_ptr->flags & FDS_TFIELD_SCOPE) == 0 || id_ptr->flags & FDS_TFIELD_MULTI_IE) {
        // Not scope field or multiple definitions
        return;
    }

    // Check non-scope fields
    static const struct opts_req_id ids_key[] = {
        {173, 0} // flowKeyIndicator
    };
    if (opts_has_required(tmptl, ids_key, ARRAY_SIZE(ids_key))) {
        // Ok, this is definitely "The Flow Keys Options Template"
        tmptl->opts_types |= FDS_OPTS_FKEYS;
    }
}

/**
 * \brief Detect Options Template type of Information Element definition
 *
 * If the type is detected, an appropriate flag will be set.
 * \note Based on RFC 5610, Section 3.9.
 * \param[in] tmplt Template structure
 */
static void
opts_detect_ietype(struct fds_template *tmplt)
{
    const uint16_t FDS_IE_IE_ID = 303; // informationElementId
    const uint16_t FDS_IE_PEN = 346; // privateEnterpriseNumber
    const struct fds_tfield *ie_id_ptr = fds_template_find(tmplt, 0, FDS_IE_IE_ID);
    const struct fds_tfield *pen_ptr = fds_template_find(tmplt, 0, FDS_IE_PEN);

    // Check scope fields
    const struct fds_tfield *ptrs[] = {ie_id_ptr, pen_ptr};
    for (size_t i = 0; i < ARRAY_SIZE(ptrs); ++i) {
        const struct fds_tfield *ptr = ptrs[i];
        if (ptr == NULL) {
            // Required item not found
            return;
        }

        if ((ptr->flags & FDS_TFIELD_SCOPE) == 0) {
            // The field found, but not in the scope!
            return;
        }

        if (ptr->flags & FDS_TFIELD_MULTI_IE) {
            // Multiple definitions are not expected!
            return;
        }
    }

    // Mandatory non-scope fields
    static const struct opts_req_id ids_type[] = {
        {339, 0}, // informationElementDataType
        {344, 0}, // informationElementSemantics
        {341, 0}  // informationElementName
    };
    if (opts_has_required(tmplt, ids_type, ARRAY_SIZE(ids_type))) {
        // Ok, this is definitely "The Information Element Type Options Template"
        tmplt->opts_types |= FDS_OPTS_IE_TYPE;
    }
}

/**
 * \brief Detect all known types of Options Template and set appropriate flags.
 * \param[in] tmplt Template structure
 */
static void
opts_detector(struct fds_template *tmplt)
{
    assert(tmplt->type == FDS_TYPE_TEMPLATE_OPTS);

    opts_detect_mproc(tmplt);
    opts_detect_eproc(tmplt);
    opts_detect_flowkey(tmplt);
    opts_detect_ietype(tmplt);
}

/**
 * \brief Create an empty template structure
 * \note All parameters are set to zero.
 * \param[in] field_cnt Number of Field Specifiers
 * \return Point to the structure or NULL.
 */
static inline struct fds_template *
template_create_empty(uint16_t field_cnt)
{
    return calloc(1, TEMPLATE_STRUCT_SIZE(field_cnt));
}

/**
 * \brief Parse a raw template header and create a new template structure
 *
 * The new template structure will be prepared for adding appropriate number of Field Specifiers
 * based on information from the raw template.
 * \param[in]      type  Type of the template
 * \param[in]      ptr   Pointer to the template header
 * \param[in, out] len   [in] Maximal length of the raw template /
 *                       [out] real length of the header of the raw template (in octets)
 * \param[out]     tmplt New template structure
 * \return On success, the function will set the parameters \p len,\p tmplt and return #FDS_OK.
 *   Otherwise, the parameters will be unchanged and the function will return #FDS_ERR_FORMAT
 *   or #FDS_ERR_NOMEM.
 */
static int
template_parse_header(enum fds_template_type type, const void *ptr, uint16_t *len,
    struct fds_template **tmplt)
{
    assert(type == FDS_TYPE_TEMPLATE || type == FDS_TYPE_TEMPLATE_OPTS);
    const size_t size_normal = sizeof(struct ipfix_template_record) - sizeof(template_ie);
    const size_t size_opts = sizeof(struct ipfix_options_template_record) - sizeof(template_ie);

    uint16_t template_id;
    uint16_t fields_total;
    uint16_t fields_scope = 0;
    uint16_t header_size = size_normal;

    if (*len < size_normal) { // the header must be at least 4 bytes long
        return FDS_ERR_FORMAT;
    }

    /*
     * Because Options Template header is superstructure of "Normal" Template header we can use it
     * also for parsing "Normal" Template. Just use only shared fields...
     */
    const struct ipfix_options_template_record *rec = ptr;
    template_id = ntohs(rec->template_id);
    if (template_id < IPFIX_SET_MIN_DATA_SET_ID) {
        return FDS_ERR_FORMAT;
    }

    fields_total = ntohs(rec->count);
    if (fields_total != 0 && type == FDS_TYPE_TEMPLATE_OPTS) {
        // It is not a withdrawal template, so it must be definitely an Options Template
        if (*len < size_opts) { // the header must be at least 6 bytes long
            return FDS_ERR_FORMAT;
        }

        header_size = size_opts;
        fields_scope = ntohs(rec->scope_field_count);
        if (fields_scope == 0 || fields_scope > fields_total) {
            return FDS_ERR_FORMAT;
        }
    }

    struct fds_template *tmplt_ptr = template_create_empty(fields_total);
    if (!tmplt_ptr) {
        return FDS_ERR_NOMEM;
    }

    tmplt_ptr->type = type;
    tmplt_ptr->id = template_id;
    tmplt_ptr->fields_cnt_total = fields_total;
    tmplt_ptr->fields_cnt_scope = fields_scope;
    *tmplt = tmplt_ptr;
    *len = header_size;
    return FDS_OK;
}

/**
 * \brief Parse Field Specifiers of a raw template
 *
 * Go through the Field Specifiers of the raw template and add them into the structure of the
 * parsed template \p tmplt.
 * \param[in]     tmplt     Template structure
 * \param[in]     field_ptr Pointer to the first specifier.
 * \param[in,out] len       [in] Maximal remaining length of the raw template /
 *                          [out] real length of the raw Field Specifiers (in octets).
 * \return On success, the function will set the parameter \p len and return #FDS_OK. Otherwise,
 *   the parameter will be unchanged and the function will return #FDS_ERR_FORMAT
 */
static int
template_parse_fields(struct fds_template *tmplt, const template_ie *field_ptr, uint16_t *len)
{
    const uint16_t field_size = sizeof(template_ie);
    const uint16_t fields_cnt = tmplt->fields_cnt_total;
    struct fds_tfield *tfield_ptr = &tmplt->fields[0];
    uint16_t len_remain = *len;

    for (uint16_t i = 0; i < fields_cnt; ++i, ++tfield_ptr, ++field_ptr, len_remain -= field_size) {
        // Parse Information Element ID
        if (len_remain < field_size) {
            // Unexpected end of the template
            return FDS_ERR_FORMAT;
        }

        tfield_ptr->length = ntohs(field_ptr->ie.length);
        tfield_ptr->id = ntohs(field_ptr->ie.id);
        if (EN_BIT_GET(tfield_ptr->id) == 0) {
            continue;
        }

        // Parse Enterprise Number
        len_remain -= field_size;
        if (len_remain < field_size) {
            // Unexpected end of the template
            return FDS_ERR_FORMAT;
        }

        ++field_ptr;
        tfield_ptr->id = EN_BIT_MASK(tfield_ptr->id);
        tfield_ptr->en = ntohl(field_ptr->enterprise_number);
    }

    *len -= len_remain;
    return FDS_OK;
}

/**
 * \brief Set feature flags of all Field Specifiers in a template
 *
 * Only ::FDS_TFIELD_SCOPE, ::FDS_TFIELD_MULTI_IE, and ::FDS_TFIELD_LAST_IE can be determined
 * based on a structure of the template. Other flags require external information.
 * \note Global flags of the template as a whole are not modified.
 * \param[in] tmplt Template structure
 */
static void
template_fields_calc_flags(struct fds_template *tmplt)
{
    const uint16_t fields_total = tmplt->fields_cnt_total;
    const uint16_t fields_scope = tmplt->fields_cnt_scope;

    // Label Scope fields
    for (uint16_t i = 0; i < fields_scope; ++i) {
        tmplt->fields[i].flags |= FDS_TFIELD_SCOPE;
    }

    // Label Multiple and Last fields
    uint64_t hash = 0;

    for (int i = fields_total - 1; i >= 0; --i) {
        struct fds_tfield *tfield_ptr = &tmplt->fields[i];

        // Calculate "hash" from IE ID
        uint64_t my_hash = (1ULL << (tfield_ptr->id % 64));
        if ((hash & my_hash) == 0) {
            // No one has the same "hash" -> this is definitely the last
            tfield_ptr->flags |= FDS_TFIELD_LAST_IE;
            hash |= my_hash;
            continue;
        }

        // Someone has the same hash. Let's check if there is exactly the same IE.
        bool same_found = false;
        for (int x = i + 1; x < fields_total; ++x) {
            struct fds_tfield *tfield_older = &tmplt->fields[x];
            if (tfield_ptr->id != tfield_older->id || tfield_ptr->en != tfield_older->en) {
                continue;
            }

            // Oh... we have a match
            tfield_ptr->flags |= FDS_TFIELD_MULTI_IE;
            tfield_older->flags |= FDS_TFIELD_MULTI_IE;
            same_found = true;
            break;
        }

        if (!same_found) {
            tfield_ptr->flags |= FDS_TFIELD_LAST_IE;
        }
    }
}

/**
 * \brief Calculate template parameters
 *
 * Feature flags of each Field Specifier will set as described in the documentation of the
 * template_fields_calc_flags() function. Regarding the global feature flags of the template,
 * only the features ::FDS_TEMPLATE_HAS_MULTI_IE and ::FDS_TEMPLATE_HAS_DYNAMIC of the template
 * will be detected and set. The expected length of appropriate data records will be calculated
 * based on the length of individual Specifiers.
 *
 * In case the template is Option Template, the function will also try to detect known type(s).
 * \param[in] tmplt Template structure
 * \return On success, returns #FDS_OK. If any parameter is not valid, the function will return
 *   #FDS_ERR_FORMAT.
 */
static int
template_calc_features(struct fds_template *tmplt)
{
    // First, calculate basic flags of each template field
    template_fields_calc_flags(tmplt);

    // Calculate flags of the whole template and each field offset in a data record
    const uint16_t fields_total = tmplt->fields_cnt_total;
    uint32_t data_len = 0; // Get (minimum) data length of a record referenced by this template
    uint16_t field_offset = 0;

    for (uint16_t i = 0; i < fields_total; ++i) {
        struct fds_tfield *field_ptr = &tmplt->fields[i];
        field_ptr->offset = field_offset;

        if (field_ptr->flags & FDS_TFIELD_MULTI_IE) {
            tmplt->flags |= FDS_TEMPLATE_HAS_MULTI_IE;
        }

        const uint16_t field_len = field_ptr->length;
        if (field_len == IPFIX_VAR_IE_LENGTH) {
            // Variable length Information Element must be at least 1 byte long
            tmplt->flags |= FDS_TEMPLATE_HAS_DYNAMIC;
            data_len += 1;
            field_offset = IPFIX_VAR_IE_LENGTH;
            continue;
        }

        data_len += field_len;
        if (field_offset != IPFIX_VAR_IE_LENGTH) {
            field_offset += field_len; // Overflow is resolved by check of total data length
        }
    }

    // Check if a record described by this templates fits into an IPFIX message
    const uint16_t max_rec_size = UINT16_MAX // Maximum length of an IPFIX message
        - sizeof(struct ipfix_header)        // IPFIX message header
        - sizeof(struct ipfix_set_header);   // IPFIX set header
    if (max_rec_size < data_len) {
        // Too long data record
        return FDS_ERR_FORMAT;
    }

    // Recognize Options Template
    if (tmplt->type == FDS_TYPE_TEMPLATE_OPTS) {
        opts_detector(tmplt);
    }

    tmplt->data_length = data_len;
    return FDS_OK;
}

/**
 * \brief Create a copy of a raw template and assign the copy to a template structure
 * \param[in] tmplt Template structure
 * \param[in] ptr   Pointer to the raw template
 * \param[in] len   Real length of the raw template
 * \return #FDS_OK or #FDS_ERR_NOMEM
 */
static inline int
template_raw_copy(struct fds_template *tmplt, const void *ptr, uint16_t len)
{
    tmplt->raw.data = (uint8_t *) malloc(len);
    if (!tmplt->raw.data) {
        return FDS_ERR_NOMEM;
    }

    memcpy(tmplt->raw.data, ptr, len);
    tmplt->raw.length = len;
    return FDS_OK;
}

int
fds_template_parse(enum fds_template_type type, const void *ptr, uint16_t *len,
    struct fds_template **tmplt)
{
    assert(type == FDS_TYPE_TEMPLATE || type == FDS_TYPE_TEMPLATE_OPTS);
    struct fds_template *template;
    uint16_t len_header, len_fields, len_real;
    int ret_code;

    // Parse a header
    len_header = *len;
    ret_code = template_parse_header(type, ptr, &len_header, &template);
    if (ret_code != FDS_OK) {
        return ret_code;
    }

    if (template->fields_cnt_total == 0) {
        // No fields... just copy the raw template
        ret_code = template_raw_copy(template, ptr, len_header);
        if (ret_code != FDS_OK) {
            fds_template_destroy(template);
            return ret_code;
        }

        *len = len_header;
        *tmplt = template;
        return FDS_OK;
    }

    // Parse fields
    const template_ie *fields_ptr = (template_ie *)(((uint8_t *) ptr) + len_header);
    len_fields = *len - len_header;
    ret_code = template_parse_fields(template, fields_ptr, &len_fields);
    if (ret_code != FDS_OK) {
        fds_template_destroy(template);
        return ret_code;
    }

    // Copy raw template
    len_real = len_header + len_fields;
    ret_code = template_raw_copy(template, ptr, len_real);
    if (ret_code != FDS_OK) {
        fds_template_destroy(template);
        return ret_code;
    }

    // Calculate features of fields and the template
    ret_code = template_calc_features(template);
    if (ret_code != FDS_OK) {
        fds_template_destroy(template);
        return ret_code;
    }

    *len = len_real;
    *tmplt = template;
    return FDS_OK;
}

struct fds_template *
fds_template_copy(const struct fds_template *tmplt)
{
    const size_t size_main = TEMPLATE_STRUCT_SIZE(tmplt->fields_cnt_total);
    const size_t size_raw = tmplt->raw.length;

    struct fds_template *cpy_main = malloc(size_main);
    uint8_t *cpy_raw = malloc(size_raw);
    if (!cpy_main || !cpy_raw) {
        free(cpy_main);
        free(cpy_raw);
        return NULL;
    }

    memcpy(cpy_main, tmplt, size_main);
    memcpy(cpy_raw, tmplt->raw.data, size_raw);
    cpy_main->raw.data = cpy_raw;
    return cpy_main;
}

void
fds_template_destroy(struct fds_template *tmplt)
{
    free(tmplt->raw.data);
    free(tmplt);
}

/**
 * \brief Determine whether an Information Element is structured or not
 * \note Structured types are defined in RFC 6313
 * \param[in] elem Information Element
 * \return True or false
 */
static inline bool
is_structured(const struct fds_iemgr_elem *elem)
{
    switch (elem->data_type) {
    case FDS_ET_BASIC_LIST:
    case FDS_ET_SUB_TEMPLATE_LIST:
    case FDS_ET_SUB_TEMPLATE_MULTILIST:
        return true;
    default:
        return false;
    }
}

const struct fds_tfield *
fds_template_cfind(const struct fds_template *tmplt, uint32_t en, uint16_t id)
{
    const uint16_t field_cnt = tmplt->fields_cnt_total;
    for (uint16_t i = 0; i < field_cnt; ++i) {
        const struct fds_tfield *ptr = &tmplt->fields[i];
        if (ptr->id != id || ptr->en != en) {
            continue;
        }

        return ptr;
    }

    return NULL;
}

struct fds_tfield *
fds_template_find(struct fds_template *tmplt, uint32_t en, uint16_t id)
{
    return (struct fds_tfield *) fds_template_cfind(tmplt, en, id);
}

/**
 * \brief Recalculate biflow specific flags
 *
 * Determined whether each field is a key or non-key field. A Biflow contains two non-key
 * fields for each value it represents associated with a single direction or endpoint: one for the
 * forward direction and one for the reverse direction. Key values are shared by both directions.
 * For more information, see RFC 5103.
 *
 * Flags that will be set: #FDS_TFIEDL_BKEY_COM, #FDS_TFIELD_BKEY_SRC, #FDS_TFIELD_BKEY_DST.
 * \warning Function expects that fields already have references to IE definitions.
 * \warning This function can be used only if at least one template field is reverse. Otherwise
 *   flags doesn't make sense.
 * \param[in] tmplt Template
 */
static void
template_ies_biflow(struct fds_template *tmplt)
{
    // Only flags
    assert(tmplt->flags & FDS_TEMPLATE_HAS_REVERSE);
    const fds_template_flag_t biflags =
        FDS_TFIELD_BKEY_SRC | FDS_TFIELD_BKEY_DST | FDS_TFIEDL_BKEY_COM;

    for (uint16_t i = 0; i < tmplt->fields_cnt_total; ++i) {
        struct fds_tfield *tfield = &tmplt->fields[i];
        assert((tfield->flags & biflags) == 0); // Flags should be cleared

        const struct fds_iemgr_elem *def = tfield->def;
        if (def != NULL) {
            if (def->is_reverse) {
                assert(tfield->flags & FDS_TFIELD_REVERSE);
                // Not common biflow field
                continue;
            }

            const struct fds_iemgr_elem *def_rev = def->reverse_elem;
            if (def_rev && fds_template_cfind(tmplt, def_rev->scope->pen, def_rev->id) != NULL) {
                // Reverse elements is present -> not common biflow field
                continue;
            }
        }

        assert((tfield->flags & FDS_TFIELD_REVERSE) == 0);
        // Failed to find reverse element -> this should be common for both direction
        tfield->flags |= FDS_TFIEDL_BKEY_COM;
        if (!def || !def->name) {
            // Only known fields can continue
            continue;
        }

        if (strncasecmp(def->name, "source", 6U) == 0) {
            tfield->flags |= FDS_TFIELD_BKEY_SRC;
        } else if (strncasecmp(def->name, "destination", 11U) == 0) {
            tfield->flags |= FDS_TFIELD_BKEY_DST;
        }
    }
}

void
fds_template_ies_define(struct fds_template *tmplt, const fds_iemgr_t *iemgr, bool preserve)
{
    if (!iemgr && preserve) {
        // Nothing to do
        return;
    }

    bool has_reverse = false;
    bool has_struct = false;

    const uint16_t fields_cnt = tmplt->fields_cnt_total;
    const struct fds_iemgr_elem *def_ptr;

    for (uint16_t i = 0; i < fields_cnt; ++i) {
        struct fds_tfield *tfield_ptr = &tmplt->fields[i];
        // Always clear all biflow specific flags
        tfield_ptr->flags &= ~(fds_template_flag_t)
            (FDS_TFIELD_BKEY_SRC | FDS_TFIELD_BKEY_DST | FDS_TFIEDL_BKEY_COM);

        if (preserve && tfield_ptr->def != NULL) {
            // Preserve this definition. Just analyse features...
            has_reverse = (tfield_ptr->flags & FDS_TFIELD_REVERSE) ? true : has_reverse;
            has_struct = (tfield_ptr->flags & FDS_TFIELD_STRUCTURED) ? true : has_struct;
            continue;
        };

        // Remove previous flags
        tfield_ptr->flags &= ~(fds_template_flag_t)(FDS_TFIELD_REVERSE | FDS_TFIELD_STRUCTURED);

        // Try to find new definition
        def_ptr = (!iemgr) ? NULL : fds_iemgr_elem_find_id(iemgr, tfield_ptr->en, tfield_ptr->id);
        if (def_ptr == NULL) {
            // Remove the old definition
            tfield_ptr->def = NULL;
            continue;
        }

        tfield_ptr->def = def_ptr;
        if (def_ptr->is_reverse) {
            tfield_ptr->flags |= FDS_TFIELD_REVERSE;
            has_reverse = true;
        }

        if (is_structured(def_ptr)) {
            tfield_ptr->flags |= FDS_TFIELD_STRUCTURED;
            has_struct = true;
        }
    }

    // Add/remove template flags
    if (has_reverse || has_struct) {
        fds_template_flag_t set_mask = 0;
        set_mask |= (has_reverse) ? FDS_TEMPLATE_HAS_REVERSE : 0;
        set_mask |= (has_struct) ? FDS_TEMPLATE_HAS_STRUCT : 0;
        tmplt->flags |= set_mask;
    }

    if (!has_reverse || !has_struct) {
        fds_template_flag_t clear_mask = 0;
        clear_mask |= (!has_reverse) ? FDS_TEMPLATE_HAS_REVERSE : 0;
        clear_mask |= (!has_struct) ? FDS_TEMPLATE_HAS_STRUCT : 0;
        tmplt->flags &= ~clear_mask;
    }

    if (has_reverse) {
        // Recalculate biflow flags
        template_ies_biflow(tmplt);
    }
}

int
fds_template_flowkey_applicable(const struct fds_template *tmplt, uint64_t flowkey)
{
    // Get the highest bit and check the correctness
    unsigned int bit_highest = 0;
    uint64_t key_cpy = flowkey;
    while (key_cpy) {
        key_cpy >>= 1;
        bit_highest++;
    }

    const uint16_t fields_cnt = tmplt->fields_cnt_total;
    if (bit_highest > fields_cnt) {
        return FDS_ERR_FORMAT;
    }

    return FDS_OK;
}

int
fds_template_flowkey_define(struct fds_template *tmplt, uint64_t flowkey)
{
    int ret_code;
    if ((ret_code = fds_template_flowkey_applicable(tmplt, flowkey)) != FDS_OK) {
        return ret_code;
    }

    if (flowkey != 0) {
        // Set the global flow key flag
        tmplt->flags |= FDS_TEMPLATE_HAS_FKEY;
    } else {
        // Clear the global flow key flag
        tmplt->flags &= ~(FDS_TEMPLATE_HAS_FKEY);
    }

    // Set flow key flags
    const uint16_t fields_cnt = tmplt->fields_cnt_total;
    for (uint16_t i = 0; i < fields_cnt; ++i, flowkey >>= 1) {
        // Add flow key flags
        if (flowkey & 0x1) {
            tmplt->fields[i].flags |= FDS_TFIELD_FLOW_KEY;
        } else {
            tmplt->fields[i].flags &= ~(FDS_TFIELD_FLOW_KEY);
        }
    }

    return FDS_OK;
}

int
fds_template_flowkey_cmp(const struct fds_template *tmplt, uint64_t flowkey)
{
    // Simple check
    bool value_exp, value_real;
    value_exp = flowkey != 0;
    value_real = (tmplt->flags & FDS_TEMPLATE_HAS_FKEY) != 0;

    if (value_exp == false && value_real == false) {
        return 0;
    } else if (value_exp != value_real) {
        return 1;
    }

    // Get the highest bit and check the correctness
    unsigned int bit_highest = 0;
    uint64_t key_cpy = flowkey;
    while (key_cpy) {
        key_cpy >>= 1;
        bit_highest++;
    }

    const uint16_t fields_cnt = tmplt->fields_cnt_total;
    if (bit_highest > fields_cnt) {
        return 1;
    }

    // Check flags
    for (uint16_t i = 0; i < fields_cnt; ++i, flowkey >>= 1) {
        value_exp = (flowkey & 0x1) != 0;
        value_real = (tmplt->fields[i].flags & FDS_TFIELD_FLOW_KEY) != 0;
        if (value_exp != value_real) {
            return 1;
        }
    }

    return 0;
}

int
fds_template_cmp(const struct fds_template *t1, const struct fds_template *t2)
{
    if (t1->raw.length != t2->raw.length) {
        return (t1->raw.length > t2->raw.length) ? 1 : (-1);
    }

    return memcmp(t1->raw.data, t2->raw.data, t1->raw.length);
}

