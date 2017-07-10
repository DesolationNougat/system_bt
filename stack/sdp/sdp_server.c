/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains functions that handle the SDP server functions.
 *  This is mainly dealing with client requests
 *
 ******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "bt_common.h"
#include "bt_types.h"
#include "bt_utils.h"
#include "bt_trace.h"
#include "btu.h"

#include "l2cdefs.h"
#include "hcidefs.h"
#include "hcimsgs.h"
#include "avrc_defs.h"

#include "sdp_api.h"
#include "sdpint.h"
#include "device/include/interop.h"
#include "btif/include/btif_storage.h"
#include "device/include/interop_config.h"
#include <errno.h>
#include <cutils/properties.h>
#include <hardware/bluetooth.h>

#if SDP_SERVER_ENABLED == TRUE

extern fixed_queue_t *btu_general_alarm_queue;

/* Maximum number of bytes to reserve out of SDP MTU for response data */
#define SDP_MAX_SERVICE_RSPHDR_LEN      12
#define SDP_MAX_SERVATTR_RSPHDR_LEN     10
#define SDP_MAX_ATTR_RSPHDR_LEN         10
#define PROFILE_VERSION_POSITION         7
#define SDP_PROFILE_DESC_LENGTH          8
#define AVRCP_SUPPORTED_FEATURES_POSITION 1
#define AVRCP_BROWSE_SUPPORT_BITMASK    0x40
#define AVRCP_CA_SUPPORT_BITMASK        0x01
#define PBAP_SKIP_GOEP_L2CAP_PSM_LEN    0x06
#define PBAP_SKIP_SUPP_FEA_LEN          0x08

/********************************************************************************/
/*              L O C A L    F U N C T I O N     P R O T O T Y P E S            */
/********************************************************************************/
static void process_service_search (tCONN_CB *p_ccb, UINT16 trans_num,
                                    UINT16 param_len, UINT8 *p_req,
                                    UINT8 *p_req_end);

static void process_service_attr_req (tCONN_CB *p_ccb, UINT16 trans_num,
                                      UINT16 param_len, UINT8 *p_req,
                                      UINT8 *p_req_end);

static void process_service_search_attr_req (tCONN_CB *p_ccb, UINT16 trans_num,
                                             UINT16 param_len, UINT8 *p_req,
                                             UINT8 *p_req_end);

static BOOLEAN is_pbap_record_blacklisted (tSDP_ATTRIBUTE attr, BD_ADDR remote_address);

static tSDP_RECORD *sdp_update_pbap_record_if_blacklisted(tSDP_RECORD *p_rec,
                                      BD_ADDR remote_address);

/********************************************************************************/
/*                  E R R O R   T E X T   S T R I N G S                         */
/*                                                                              */
/* The default is to have no text string, but we allow the strings to be        */
/* configured in target.h if people want them.                                  */
/********************************************************************************/
#ifndef SDP_TEXT_BAD_HEADER
#define SDP_TEXT_BAD_HEADER     NULL
#endif

#ifndef SDP_TEXT_BAD_PDU
#define SDP_TEXT_BAD_PDU        NULL
#endif

#ifndef SDP_TEXT_BAD_UUID_LIST
#define SDP_TEXT_BAD_UUID_LIST  NULL
#endif

#ifndef SDP_TEXT_BAD_HANDLE
#define SDP_TEXT_BAD_HANDLE     NULL
#endif

#ifndef SDP_TEXT_BAD_ATTR_LIST
#define SDP_TEXT_BAD_ATTR_LIST  NULL
#endif

#ifndef SDP_TEXT_BAD_CONT_LEN
#define SDP_TEXT_BAD_CONT_LEN   NULL
#endif

#ifndef SDP_TEXT_BAD_CONT_INX
#define SDP_TEXT_BAD_CONT_INX   NULL
#endif

#ifndef SDP_TEXT_BAD_MAX_RECORDS_LIST
#define SDP_TEXT_BAD_MAX_RECORDS_LIST   NULL
#endif

struct blacklist_entry
{
    int ver;
    char addr[3];
};

int sdp_get_stored_avrc_tg_version(BD_ADDR addr)
{
    int stored_ver = AVRC_REV_INVALID;
    struct blacklist_entry data;
    FILE *fp;

    SDP_TRACE_DEBUG("%s target BD Addr: %x:%x:%x", __func__,\
                        addr[0], addr[1], addr[2]);

    fp = fopen(AVRC_PEER_VERSION_CONF_FILE, "rb");
    if (!fp)
    {
       SDP_TRACE_ERROR("%s unable to open AVRC Conf file for read: err: (%s)",\
                                        __func__, strerror(errno));
       return stored_ver;
    }
    while (fread(&data, sizeof(data), 1, fp) != 0)
    {
        SDP_TRACE_DEBUG("Entry: addr = %x:%x:%x, ver = 0x%x",\
                data.addr[0], data.addr[1], data.addr[2], data.ver);
        if(!memcmp(addr, data.addr, 3))
        {
            stored_ver = data.ver;
            SDP_TRACE_DEBUG("Entry found with version: 0x%x", stored_ver);
            break;
        }
    }
    fclose(fp);
    return stored_ver;
}

/****************************************************************************
**
** Function         sdp_dev_blacklisted_for_avrcp15
**
** Description      This function is called to check if Remote device
**                  is blacklisted for Avrcp version.
**
** Returns          BOOLEAN
**
*******************************************************************************/
BOOLEAN sdp_dev_blacklisted_for_avrcp15 (BD_ADDR addr)
{
    bt_bdaddr_t remote_bdaddr;
    bdcpy(remote_bdaddr.address, addr);

    if (interop_match_addr(INTEROP_ADV_AVRCP_VER_1_3, &remote_bdaddr)) {
        bt_property_t prop_name;
        bt_bdname_t bdname;

        BTIF_STORAGE_FILL_PROPERTY(&prop_name, BT_PROPERTY_BDNAME,
                               sizeof(bt_bdname_t), &bdname);
        if (btif_storage_get_remote_device_property(&remote_bdaddr,
                                              &prop_name) != BT_STATUS_SUCCESS)
        {
            SDP_TRACE_ERROR("%s: BT_PROPERTY_BDNAME failed, returning false", __func__);
            return FALSE;
        }

        if (strlen((const char *)bdname.name) != 0 &&
            interop_match_name(INTEROP_ADV_AVRCP_VER_1_3, (const char *)bdname.name))
        {
            SDP_TRACE_DEBUG("%s: advertise AVRCP version 1.3 for device", __func__);
            return TRUE;
        }
    }

    return FALSE;
}

/*************************************************************************************
**
** Function        sdp_fallback_avrcp_version
**
** Description     Checks if UUID is AV Remote Control, attribute id
**                 is Profile descriptor list and remote BD address
**                 matches device blacklist, change Avrcp version to 1.3
**
** Returns         BOOLEAN
**
***************************************************************************************/
BOOLEAN sdp_fallback_avrcp_version (tSDP_ATTRIBUTE *p_attr, BD_ADDR remote_address)
{
    char a2dp_role[PROPERTY_VALUE_MAX] = "false";
    if ((p_attr->id == ATTR_ID_BT_PROFILE_DESC_LIST) &&
        (p_attr->len >= SDP_PROFILE_DESC_LENGTH))
    {
        /* As per current DB implementation UUID is condidered as 16 bit */
        if (((p_attr->value_ptr[3] << 8) | (p_attr->value_ptr[4])) ==
                UUID_SERVCLASS_AV_REMOTE_CONTROL)
        {
            int ver;
            if (sdp_dev_blacklisted_for_avrcp15 (remote_address))
            {
                p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x03; // Update AVRCP version as 1.3
                SDP_TRACE_ERROR("SDP Change AVRCP Version = 0x%x",
                         p_attr->value_ptr[PROFILE_VERSION_POSITION]);
                return TRUE;
            }
            property_get("persist.service.bt.a2dp.sink", a2dp_role, "false");
            if (!strncmp("false", a2dp_role, 5)) {
                ver = sdp_get_stored_avrc_tg_version (remote_address);
                if (ver != AVRC_REV_INVALID)
                {
                    SDP_TRACE_DEBUG("Stored AVRC TG version: 0x%x", ver);
                    p_attr->value_ptr[PROFILE_VERSION_POSITION] = (UINT8)(ver & 0x00ff);
                    SDP_TRACE_DEBUG("SDP Change AVRCP Version = 0x%x",
                                 p_attr->value_ptr[PROFILE_VERSION_POSITION]);
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
                    if (ver != AVRC_REV_1_6)
#else
#if (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE))
                    if (ver != AVRC_REV_1_5)
#endif
#endif
                        return TRUE;
                    else
                        return FALSE;
                }
                else
                {
                    p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x03; // Update AVRCP ver as 1.3
                    SDP_TRACE_DEBUG("Device not stored, Change AVRCP Version = 0x%x",
                             p_attr->value_ptr[PROFILE_VERSION_POSITION]);
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/*************************************************************************************
**
** Function        sdp_reset_avrcp_browsing_bit
**
** Description     Checks if Service Class ID is AV Remote Control TG, attribute id
**                 is Supported features and remote BD address
**                 matches device blacklist, reset Browsing Bit
**
** Returns         BOOLEAN
**
***************************************************************************************/
BOOLEAN sdp_reset_avrcp_browsing_bit (tSDP_ATTRIBUTE attr, tSDP_ATTRIBUTE *p_attr,
BD_ADDR                                                                      remote_address)
{
    if ((p_attr->id == ATTR_ID_SUPPORTED_FEATURES) && (attr.id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
        (((attr.value_ptr[1] << 8) | (attr.value_ptr[2])) == UUID_SERVCLASS_AV_REM_CTRL_TARGET))
    {
        int ver;
        if (sdp_dev_blacklisted_for_avrcp15 (remote_address))
        {
            SDP_TRACE_ERROR("Reset Browse feature bitmask");
            p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION] &= ~AVRCP_BROWSE_SUPPORT_BITMASK;
            return TRUE;
        }
        ver = sdp_get_stored_avrc_tg_version (remote_address);
        SDP_TRACE_ERROR("Stored AVRC TG version: 0x%x", ver);
        if ((ver < AVRC_REV_1_4) || (ver == AVRC_REV_INVALID))
        {
            SDP_TRACE_ERROR("Reset Browse feature bitmask");
            p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION] &= ~AVRCP_BROWSE_SUPPORT_BITMASK;
            return TRUE;
        }
    }
    return FALSE;
}

/*************************************************************************************
**
** Function        sdp_change_hfp_version
**
** Description     Checks if UUID is AG_HANDSFREE, attribute id
**                 is Profile descriptor list and remote BD address
**                 matches device blacklist, change hfp version to 1.7
**
** Returns         BOOLEAN
**
***************************************************************************************/
BOOLEAN sdp_change_hfp_version (tSDP_ATTRIBUTE *p_attr, BD_ADDR remote_address)
{
    bool is_blacklisted = FALSE;
    char value[PROPERTY_VALUE_MAX];
    if ((p_attr->id == ATTR_ID_BT_PROFILE_DESC_LIST) &&
        (p_attr->len >= SDP_PROFILE_DESC_LENGTH))
    {
        /* As per current DB implementation UUID is condidered as 16 bit */
        if (((p_attr->value_ptr[3] << 8) | (p_attr->value_ptr[4])) ==
                UUID_SERVCLASS_HF_HANDSFREE)
        {
            bt_bdaddr_t remote_bdaddr;
            bdcpy(remote_bdaddr.address, remote_address);
            is_blacklisted = interop_database_match_addr(INTEROP_HFP_1_7_BLACKLIST, (bt_bdaddr_t *)&remote_bdaddr);
            SDP_TRACE_DEBUG("%s: HF version is 1.7 for BD addr: %x:%x:%x",\
                           __func__, remote_address[0], remote_address[1], remote_address[2]);
            /* For PTS we should show AG's HFP version as 1.7 */
            if (is_blacklisted ||
                (property_get("bt.pts.certification", value, "false") &&
                 strcmp(value, "true") == 0))
            {
                p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x07; // Update HFP version as 1.7
                SDP_TRACE_ERROR("SDP Change HFP Version = 0x%x",
                         p_attr->value_ptr[PROFILE_VERSION_POSITION]);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/*************************************************************************************
**
** Function        sdp_reset_avrcp_cover_art_bit
**
** Description     Checks if Service Class ID is AV Remote Control TG, attribute id
**                 is Supported features and remote BD address
**                 matches device blacklist, reset Cover Art Bit
**
** Returns         BOOLEAN
**
***************************************************************************************/

BOOLEAN sdp_reset_avrcp_cover_art_bit (tSDP_ATTRIBUTE attr, tSDP_ATTRIBUTE *p_attr,
                                                 BD_ADDR remote_address)
{
    if ((p_attr->id == ATTR_ID_SUPPORTED_FEATURES) && (attr.id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
        (((attr.value_ptr[1] << 8) | (attr.value_ptr[2])) == UUID_SERVCLASS_AV_REM_CTRL_TARGET))
    {
        int ver;
        ver = sdp_get_stored_avrc_tg_version (remote_address);
        SDP_TRACE_ERROR("Stored AVRC TG version: 0x%x", ver);
        if ((ver < AVRC_REV_1_6) || (ver == AVRC_REV_INVALID))
        {
            SDP_TRACE_ERROR("Reset Cover Art feature bitmask +1, 0x%x", p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION+1]);
            SDP_TRACE_ERROR("Reset Cover Art feature bitmask -1, 0x%x", p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION-1]);
            p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION-1] &= ~AVRCP_CA_SUPPORT_BITMASK;
            SDP_TRACE_ERROR("Reset Cover Art feature bitmask, new -1, 0x%x", p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION-1]);
            return TRUE;
        }
    }
    return FALSE;
}

/*******************************************************************************
**
** Function         sdp_server_handle_client_req
**
** Description      This is the main dispatcher of the SDP server. It is called
**                  when any data is received from L2CAP, and dispatches the
**                  request to the appropriate handler.
**
** Returns          void
**
*******************************************************************************/
void sdp_server_handle_client_req (tCONN_CB *p_ccb, BT_HDR *p_msg)
{
    UINT8   *p_req     = (UINT8 *) (p_msg + 1) + p_msg->offset;
    UINT8   *p_req_end = p_req + p_msg->len;
    UINT8   pdu_id;
    UINT16  trans_num, param_len;


    /* Start inactivity timer */
    alarm_set_on_queue(p_ccb->sdp_conn_timer, SDP_INACT_TIMEOUT_MS,
                       sdp_conn_timer_timeout, p_ccb, btu_general_alarm_queue);

    /* The first byte in the message is the pdu type */
    pdu_id = *p_req++;

    /* Extract the transaction number and parameter length */
    BE_STREAM_TO_UINT16 (trans_num, p_req);
    BE_STREAM_TO_UINT16 (param_len, p_req);

    if ((p_req + param_len) != p_req_end)
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
        return;
    }

    switch (pdu_id)
    {
    case SDP_PDU_SERVICE_SEARCH_REQ:
        process_service_search (p_ccb, trans_num, param_len, p_req, p_req_end);
        break;

    case SDP_PDU_SERVICE_ATTR_REQ:
        process_service_attr_req (p_ccb, trans_num, param_len, p_req, p_req_end);
        break;

    case SDP_PDU_SERVICE_SEARCH_ATTR_REQ:
        process_service_search_attr_req (p_ccb, trans_num, param_len, p_req, p_req_end);
        break;

    default:
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_REQ_SYNTAX, SDP_TEXT_BAD_PDU);
        SDP_TRACE_WARNING ("SDP - server got unknown PDU: 0x%x", pdu_id);
        break;
    }
}



/*******************************************************************************
**
** Function         process_service_search
**
** Description      This function handles a service search request from the
**                  client. It builds a reply message with info from the database,
**                  and sends the reply back to the client.
**
** Returns          void
**
*******************************************************************************/
static void process_service_search (tCONN_CB *p_ccb, UINT16 trans_num,
                                    UINT16 param_len, UINT8 *p_req,
                                    UINT8 *p_req_end)
{
    UINT16          max_replies, cur_handles, rem_handles, cont_offset;
    tSDP_UUID_SEQ   uid_seq;
    UINT8           *p_rsp, *p_rsp_start, *p_rsp_param_len;
    UINT16          rsp_param_len, num_rsp_handles, xx;
    UINT32          rsp_handles[SDP_MAX_RECORDS] = {0};
    tSDP_RECORD    *p_rec = NULL;
    BOOLEAN         is_cont = FALSE;
    UNUSED(p_req_end);

    p_req = sdpu_extract_uid_seq (p_req, param_len, &uid_seq);

    if ((!p_req) || (!uid_seq.num_uids))
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_REQ_SYNTAX, SDP_TEXT_BAD_UUID_LIST);
        return;
    }

    /* Get the max replies we can send. Cap it at our max anyways. */
    BE_STREAM_TO_UINT16 (max_replies, p_req);

    if (max_replies > SDP_MAX_RECORDS)
        max_replies = SDP_MAX_RECORDS;


    if ((!p_req) || (p_req > p_req_end))
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_REQ_SYNTAX, SDP_TEXT_BAD_MAX_RECORDS_LIST);
        return;
    }


    /* Get a list of handles that match the UUIDs given to us */
    for (num_rsp_handles = 0; num_rsp_handles < max_replies; )
    {
        p_rec = sdp_db_service_search (p_rec, &uid_seq);

        if (p_rec)
            rsp_handles[num_rsp_handles++] = p_rec->record_handle;
        else
            break;
    }

    /* Check if this is a continuation request */
    if (*p_req)
    {
        if (*p_req++ != SDP_CONTINUATION_LEN || (p_req >= p_req_end))
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_CONT_STATE,
                                     SDP_TEXT_BAD_CONT_LEN);
            return;
        }
        BE_STREAM_TO_UINT16 (cont_offset, p_req);

        if (cont_offset != p_ccb->cont_offset)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_CONT_STATE,
                                     SDP_TEXT_BAD_CONT_INX);
            return;
        }

        if (p_req != p_req_end)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
            return;
        }
        rem_handles = num_rsp_handles - cont_offset;    /* extract the remaining handles */
    }
    else
    {
        if (p_req+1 != p_req_end)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
            return;
        }
        rem_handles = num_rsp_handles;
        cont_offset = 0;
        p_ccb->cont_offset = 0;
    }

    /* Calculate how many handles will fit in one PDU */
    cur_handles = (UINT16)((p_ccb->rem_mtu_size - SDP_MAX_SERVICE_RSPHDR_LEN) / 4);

    if (rem_handles <= cur_handles)
        cur_handles = rem_handles;
    else /* Continuation is set */
    {
        p_ccb->cont_offset += cur_handles;
        is_cont = TRUE;
    }

    /* Get a buffer to use to build the response */
    BT_HDR *p_buf = (BT_HDR *)osi_malloc(SDP_DATA_BUF_SIZE);
    p_buf->offset = L2CAP_MIN_OFFSET;
    p_rsp = p_rsp_start = (UINT8 *)(p_buf + 1) + L2CAP_MIN_OFFSET;

    /* Start building a rsponse */
    UINT8_TO_BE_STREAM  (p_rsp, SDP_PDU_SERVICE_SEARCH_RSP);
    UINT16_TO_BE_STREAM (p_rsp, trans_num);

    /* Skip the length, we need to add it at the end */
    p_rsp_param_len = p_rsp;
    p_rsp += 2;

    /* Put in total and current number of handles, and handles themselves */
    UINT16_TO_BE_STREAM (p_rsp, num_rsp_handles);
    UINT16_TO_BE_STREAM (p_rsp, cur_handles);

/*    SDP_TRACE_DEBUG("SDP Service Rsp: tothdl %d, curhdlr %d, start %d, end %d, cont %d",
                     num_rsp_handles, cur_handles, cont_offset,
                     cont_offset + cur_handles-1, is_cont); */
    for (xx = cont_offset; xx < cont_offset + cur_handles; xx++)
        UINT32_TO_BE_STREAM (p_rsp, rsp_handles[xx]);

    if (is_cont)
    {
        UINT8_TO_BE_STREAM  (p_rsp, SDP_CONTINUATION_LEN);
        UINT16_TO_BE_STREAM (p_rsp, p_ccb->cont_offset);
    }
    else
        UINT8_TO_BE_STREAM (p_rsp, 0);

    /* Go back and put the parameter length into the buffer */
    rsp_param_len = p_rsp - p_rsp_param_len - 2;
    UINT16_TO_BE_STREAM (p_rsp_param_len, rsp_param_len);

    /* Set the length of the SDP data in the buffer */
    p_buf->len = p_rsp - p_rsp_start;


    /* Send the buffer through L2CAP */
    L2CA_DataWrite (p_ccb->connection_id, p_buf);
}


/*******************************************************************************
**
** Function         process_service_attr_req
**
** Description      This function handles an attribute request from the client.
**                  It builds a reply message with info from the database,
**                  and sends the reply back to the client.
**
** Returns          void
**
*******************************************************************************/
static void process_service_attr_req (tCONN_CB *p_ccb, UINT16 trans_num,
                                      UINT16 param_len, UINT8 *p_req,
                                      UINT8 *p_req_end)
{
    UINT16          max_list_len, len_to_send, cont_offset;
    INT16           rem_len;
    tSDP_ATTR_SEQ   attr_seq, attr_seq_sav;
    UINT8           *p_rsp, *p_rsp_start, *p_rsp_param_len;
    UINT16          rsp_param_len, xx;
    UINT32          rec_handle;
    tSDP_RECORD     *p_rec;
    tSDP_ATTRIBUTE  *p_attr;
    BOOLEAN         is_cont = FALSE;
    BOOLEAN         is_avrcp_fallback = FALSE;
    BOOLEAN         is_avrcp_browse_bit_reset = FALSE;
    BOOLEAN         is_hfp_fallback = FALSE;
    BOOLEAN         is_avrcp_ca_bit_reset = FALSE;
    UINT16          attr_len;

    /* Extract the record handle */
    BE_STREAM_TO_UINT32 (rec_handle, p_req);

    if (p_req > p_req_end)
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_SERV_REC_HDL, SDP_TEXT_BAD_HANDLE);
        return;
    }

    /* Get the max list length we can send. Cap it at MTU size minus overhead */
    BE_STREAM_TO_UINT16 (max_list_len, p_req);

    if (max_list_len > (p_ccb->rem_mtu_size - SDP_MAX_ATTR_RSPHDR_LEN))
        max_list_len = p_ccb->rem_mtu_size - SDP_MAX_ATTR_RSPHDR_LEN;

    p_req = sdpu_extract_attr_seq (p_req, param_len, &attr_seq);

    if ((!p_req) || (!attr_seq.num_attr) || (p_req > p_req_end))
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_REQ_SYNTAX, SDP_TEXT_BAD_ATTR_LIST);
        return;
    }

    memcpy(&attr_seq_sav, &attr_seq, sizeof(tSDP_ATTR_SEQ)) ;

    /* Find a record with the record handle */
    p_rec = sdp_db_find_record (rec_handle);
    if (!p_rec)
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_SERV_REC_HDL, SDP_TEXT_BAD_HANDLE);
        return;
    }

    p_rec = sdp_update_pbap_record_if_blacklisted(p_rec, p_ccb->device_address);

    /* Free and reallocate buffer */
    osi_free(p_ccb->rsp_list);
    p_ccb->rsp_list = (UINT8 *)osi_malloc(max_list_len);

    /* Check if this is a continuation request */
    if (*p_req) {
        if (*p_req++ != SDP_CONTINUATION_LEN) {
            sdpu_build_n_send_error(p_ccb, trans_num, SDP_INVALID_CONT_STATE,
                                    SDP_TEXT_BAD_CONT_LEN);
            return;
        }
        BE_STREAM_TO_UINT16(cont_offset, p_req);

        if (cont_offset != p_ccb->cont_offset) {
            sdpu_build_n_send_error(p_ccb, trans_num, SDP_INVALID_CONT_STATE,
                                    SDP_TEXT_BAD_CONT_INX);
            return;
        }
        if (p_req != p_req_end)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
            return;
        }
        is_cont = TRUE;

        /* Initialise for continuation response */
        p_rsp = &p_ccb->rsp_list[0];
        attr_seq.attr_entry[p_ccb->cont_info.next_attr_index].start =
            p_ccb->cont_info.next_attr_start_id;
    } else {
        if (p_req+1 != p_req_end)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
            return;
        }

        p_ccb->cont_offset = 0;
        p_rsp = &p_ccb->rsp_list[3];    /* Leave space for data elem descr */

        /* Reset continuation parameters in p_ccb */
        p_ccb->cont_info.prev_sdp_rec = NULL;
        p_ccb->cont_info.curr_sdp_rec = NULL;
        p_ccb->cont_info.next_attr_index = 0;
        p_ccb->cont_info.attr_offset = 0;
    }

    /* Search for attributes that match the list given to us */
    for (xx = p_ccb->cont_info.next_attr_index; xx < attr_seq.num_attr; xx++)
    {
        p_attr = sdp_db_find_attr_in_rec (p_rec, attr_seq.attr_entry[xx].start, attr_seq.attr_entry[xx].end);

        if (p_attr)
        {
#if ((defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE)) || \
        (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE)))
            /* Check for UUID Remote Control and Remote BD address  */
            is_avrcp_fallback = sdp_fallback_avrcp_version (p_attr, p_ccb->device_address);
#if (defined(AVCT_BROWSE_INCLUDED)&&(AVCT_BROWSE_INCLUDED == TRUE))
            is_avrcp_browse_bit_reset = sdp_reset_avrcp_browsing_bit(
                        p_rec->attribute[1], p_attr, p_ccb->device_address);
#endif
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
            is_avrcp_ca_bit_reset = sdp_reset_avrcp_cover_art_bit(
                        p_rec->attribute[1], p_attr, p_ccb->device_address);
#endif
#endif
            is_hfp_fallback = sdp_change_hfp_version (p_attr, p_ccb->device_address);
            /* Check if attribute fits. Assume 3-byte value type/length */
            rem_len = max_list_len - (INT16) (p_rsp - &p_ccb->rsp_list[0]);

            /* just in case */
            if (rem_len <= 0)
            {
                p_ccb->cont_info.next_attr_index = xx;
                p_ccb->cont_info.next_attr_start_id = p_attr->id;
                break;
            }

            attr_len = sdpu_get_attrib_entry_len(p_attr);
            /* if there is a partial attribute pending to be sent */
            if (p_ccb->cont_info.attr_offset)
            {
                p_rsp = sdpu_build_partial_attrib_entry (p_rsp, p_attr, rem_len,
                                                         &p_ccb->cont_info.attr_offset);

                /* If the partial attrib could not been fully added yet */
                if (p_ccb->cont_info.attr_offset != attr_len)
                    break;
                else /* If the partial attrib has been added in full by now */
                    p_ccb->cont_info.attr_offset = 0; /* reset attr_offset */
            }
            else if (rem_len < attr_len) /* Not enough space for attr... so add partially */
            {
                if (attr_len >= SDP_MAX_ATTR_LEN)
                {
                    SDP_TRACE_ERROR("SDP attr too big: max_list_len=%d,attr_len=%d", max_list_len, attr_len);
                    sdpu_build_n_send_error (p_ccb, trans_num, SDP_NO_RESOURCES, NULL);
                    return;
                }

                /* add the partial attribute if possible */
                p_rsp = sdpu_build_partial_attrib_entry (p_rsp, p_attr, (UINT16)rem_len,
                                                         &p_ccb->cont_info.attr_offset);

                p_ccb->cont_info.next_attr_index = xx;
                p_ccb->cont_info.next_attr_start_id = p_attr->id;
                break;
            }
            else /* build the whole attribute */
                p_rsp = sdpu_build_attrib_entry (p_rsp, p_attr);

            /* If doing a range, stick with this one till no more attributes found */
            if (attr_seq.attr_entry[xx].start != attr_seq.attr_entry[xx].end)
            {
                /* Update for next time through */
                attr_seq.attr_entry[xx].start = p_attr->id + 1;

                xx--;
            }
            if (is_avrcp_fallback)
            {
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
                /* Update AVRCP version back to 1.6 */
                p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
#else
#if (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE))
                /* Update AVRCP version back to 1.5 */
                p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x05;
#endif
#endif
                is_avrcp_fallback = FALSE;
            }
            if (is_avrcp_browse_bit_reset)
            {
                /* Restore Browsing bit */
                SDP_TRACE_ERROR("Restore Browsing bit");
                p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION]
                                        |= AVRCP_BROWSE_SUPPORT_BITMASK;
                is_avrcp_browse_bit_reset = FALSE;
            }
            if (is_hfp_fallback)
            {
                SDP_TRACE_ERROR("Restore HFP version to 1.6");
                /* Update HFP version back to 1.6 */
                p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
                is_hfp_fallback = FALSE;
            }
            if (is_avrcp_ca_bit_reset)
            {
                /* Restore Cover Art bit */
                SDP_TRACE_ERROR("Restore Cover Art bit");
                p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION - 1]
                                        |= AVRCP_CA_SUPPORT_BITMASK;
                is_avrcp_ca_bit_reset = FALSE;
            }
        }
    }
    if (is_avrcp_fallback)
    {
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
        /* Update AVRCP version back to 1.6 */
        p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
#else
#if (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE))
        /* Update AVRCP version back to 1.5 */
        p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x05;
#endif
#endif
        is_avrcp_fallback = FALSE;
    }
    if (is_avrcp_browse_bit_reset)
    {
        /* Restore Browsing bit */
        SDP_TRACE_ERROR("Restore Browsing bit");
        p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION]
                                    |= AVRCP_BROWSE_SUPPORT_BITMASK;
        is_avrcp_browse_bit_reset = FALSE;
    }
    if (is_hfp_fallback)
    {
        SDP_TRACE_ERROR("Restore HFP version to 1.6");
        /* Update HFP version back to 1.6 */
        p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
        is_hfp_fallback = FALSE;
    }
    if (is_avrcp_ca_bit_reset)
    {
        /* Restore Cover Art bit */
        SDP_TRACE_ERROR("Restore Cover Art bit");
        p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION - 1]
                                |= AVRCP_CA_SUPPORT_BITMASK;
        is_avrcp_ca_bit_reset = FALSE;
    }
    /* If all the attributes have been accomodated in p_rsp,
       reset next_attr_index */
    if (xx == attr_seq.num_attr)
        p_ccb->cont_info.next_attr_index = 0;

    len_to_send = (UINT16) (p_rsp - &p_ccb->rsp_list[0]);
    cont_offset = 0;

    if (!is_cont)
    {
        p_ccb->list_len = sdpu_get_attrib_seq_len(p_rec, &attr_seq_sav) + 3;
        /* Put in the sequence header (2 or 3 bytes) */
        if (p_ccb->list_len > 255)
        {
            p_ccb->rsp_list[0] = (UINT8) ((DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_WORD);
            p_ccb->rsp_list[1] = (UINT8) ((p_ccb->list_len - 3) >> 8);
            p_ccb->rsp_list[2] = (UINT8) (p_ccb->list_len - 3);
        }
        else
        {
            cont_offset = 1;

            p_ccb->rsp_list[1] = (UINT8) ((DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);
            p_ccb->rsp_list[2] = (UINT8) (p_ccb->list_len - 3);

            p_ccb->list_len--;
            len_to_send--;
        }
    }

    /* Get a buffer to use to build the response */
    BT_HDR *p_buf = (BT_HDR *)osi_malloc(SDP_DATA_BUF_SIZE);
    p_buf->offset = L2CAP_MIN_OFFSET;
    p_rsp = p_rsp_start = (UINT8 *)(p_buf + 1) + L2CAP_MIN_OFFSET;

    /* Start building a rsponse */
    UINT8_TO_BE_STREAM  (p_rsp, SDP_PDU_SERVICE_ATTR_RSP);
    UINT16_TO_BE_STREAM (p_rsp, trans_num);

    /* Skip the parameter length, add it when we know the length */
    p_rsp_param_len = p_rsp;
    p_rsp += 2;

    UINT16_TO_BE_STREAM (p_rsp, len_to_send);

    memcpy (p_rsp, &p_ccb->rsp_list[cont_offset], len_to_send);
    p_rsp += len_to_send;

    p_ccb->cont_offset += len_to_send;

    /* If anything left to send, continuation needed */
    if (p_ccb->cont_offset < p_ccb->list_len)
    {
        is_cont = TRUE;

        UINT8_TO_BE_STREAM  (p_rsp, SDP_CONTINUATION_LEN);
        UINT16_TO_BE_STREAM (p_rsp, p_ccb->cont_offset);
    }
    else
        UINT8_TO_BE_STREAM (p_rsp, 0);

    /* Go back and put the parameter length into the buffer */
    rsp_param_len = p_rsp - p_rsp_param_len - 2;
    UINT16_TO_BE_STREAM (p_rsp_param_len, rsp_param_len);

    /* Set the length of the SDP data in the buffer */
    p_buf->len = p_rsp - p_rsp_start;


    /* Send the buffer through L2CAP */
    L2CA_DataWrite (p_ccb->connection_id, p_buf);
}



/*******************************************************************************
**
** Function         process_service_search_attr_req
**
** Description      This function handles a combined service search and attribute
**                  read request from the client. It builds a reply message with
**                  info from the database, and sends the reply back to the client.
**
** Returns          void
**
*******************************************************************************/
static void process_service_search_attr_req (tCONN_CB *p_ccb, UINT16 trans_num,
                                             UINT16 param_len, UINT8 *p_req,
                                             UINT8 *p_req_end)
{
    UINT16         max_list_len;
    INT16          rem_len;
    UINT16         len_to_send, cont_offset;
    tSDP_UUID_SEQ   uid_seq;
    UINT8           *p_rsp, *p_rsp_start, *p_rsp_param_len;
    UINT16          rsp_param_len, xx;
    tSDP_RECORD    *p_rec;
    tSDP_RECORD    *p_prev_rec;
    tSDP_ATTR_SEQ   attr_seq, attr_seq_sav;
    tSDP_ATTRIBUTE *p_attr;
    BT_HDR         *p_buf;
    BOOLEAN         maxxed_out = FALSE, is_cont = FALSE;
    BOOLEAN         is_avrcp_fallback = FALSE;
    BOOLEAN         is_avrcp_browse_bit_reset = FALSE;
    BOOLEAN         is_hfp_fallback = FALSE;
    BOOLEAN         is_avrcp_ca_bit_reset = FALSE;
    UINT8           *p_seq_start = NULL;
    UINT16          seq_len, attr_len;
    UINT16          blacklist_skip_len = 0;
    UNUSED(p_req_end);

    /* Extract the UUID sequence to search for */
    p_req = sdpu_extract_uid_seq (p_req, param_len, &uid_seq);

    if ((!p_req) || (!uid_seq.num_uids))
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_REQ_SYNTAX, SDP_TEXT_BAD_UUID_LIST);
        return;
    }

    /* Get the max list length we can send. Cap it at our max list length. */
    BE_STREAM_TO_UINT16 (max_list_len, p_req);

    if (max_list_len > (p_ccb->rem_mtu_size - SDP_MAX_SERVATTR_RSPHDR_LEN))
        max_list_len = p_ccb->rem_mtu_size - SDP_MAX_SERVATTR_RSPHDR_LEN;

    p_req = sdpu_extract_attr_seq (p_req, param_len, &attr_seq);

    if ((!p_req) || (!attr_seq.num_attr))
    {
        sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_REQ_SYNTAX, SDP_TEXT_BAD_ATTR_LIST);
        return;
    }

    memcpy(&attr_seq_sav, &attr_seq, sizeof(tSDP_ATTR_SEQ)) ;
    /* Free and reallocate buffer */
    osi_free(p_ccb->rsp_list);
    p_ccb->rsp_list = (UINT8 *)osi_malloc(max_list_len);

    /* Check if this is a continuation request */
    if (*p_req) {
        if (*p_req++ != SDP_CONTINUATION_LEN) {
            sdpu_build_n_send_error(p_ccb, trans_num, SDP_INVALID_CONT_STATE,
                                    SDP_TEXT_BAD_CONT_LEN);
            return;
        }
        BE_STREAM_TO_UINT16(cont_offset, p_req);

        if (cont_offset != p_ccb->cont_offset) {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_CONT_STATE,
                                     SDP_TEXT_BAD_CONT_INX);
            return;
        }
        if (p_req != p_req_end)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
            return;
        }
        is_cont = TRUE;

        /* Initialise for continuation response */
        p_rsp = &p_ccb->rsp_list[0];
        attr_seq.attr_entry[p_ccb->cont_info.next_attr_index].start =
            p_ccb->cont_info.next_attr_start_id;
    } else {
        if (p_req+1 != p_req_end)
        {
            sdpu_build_n_send_error (p_ccb, trans_num, SDP_INVALID_PDU_SIZE, SDP_TEXT_BAD_HEADER);
            return;
        }

        p_ccb->cont_offset = 0;
        p_rsp = &p_ccb->rsp_list[3];    /* Leave space for data elem descr */

        /* Reset continuation parameters in p_ccb */
        p_ccb->cont_info.prev_sdp_rec = NULL;
        p_ccb->cont_info.curr_sdp_rec = NULL;
        p_ccb->cont_info.next_attr_index = 0;
        p_ccb->cont_info.last_attr_seq_desc_sent = FALSE;
        p_ccb->cont_info.attr_offset = 0;
    }

    /* Get a list of handles that match the UUIDs given to us */
    for (p_rec = sdp_db_service_search (p_ccb->cont_info.prev_sdp_rec, &uid_seq); p_rec; p_rec = sdp_db_service_search (p_rec, &uid_seq))
    {
        p_ccb->cont_info.curr_sdp_rec = p_rec;
        /* Store the actual record pointer which would be reused later */
        p_prev_rec = p_rec;
        p_rec = sdp_update_pbap_record_if_blacklisted(p_rec, p_ccb->device_address);
        if (p_rec != p_prev_rec) {
            /* Remote device is blacklisted for PBAP, calculate the reduction in length */
            for (xx = p_ccb->cont_info.next_attr_index; xx < attr_seq_sav.num_attr; xx++) {
                if (attr_seq_sav.attr_entry[xx].start == attr_seq_sav.attr_entry[xx].end) {
                    if (attr_seq_sav.attr_entry[xx].start == ATTR_ID_GOEP_L2CAP_PSM) {
                        blacklist_skip_len += PBAP_SKIP_GOEP_L2CAP_PSM_LEN;
                        SDP_TRACE_ERROR("%s: ATTR_ID_GOEP_L2CAP_PSM requested,"
                            " need to reduce length by %d", __func__,
                            blacklist_skip_len);
                    } else if (attr_seq_sav.attr_entry[xx].start ==
                        ATTR_ID_PBAP_SUPPORTED_FEATURES) {
                        blacklist_skip_len += PBAP_SKIP_SUPP_FEA_LEN;
                        SDP_TRACE_DEBUG("%s: ATTR_ID_PBAP_SUPPORTED_FEATURES requested,"
                            " need to reduce length by %d", __func__,
                            blacklist_skip_len);
                    }
                } else {
                    blacklist_skip_len = PBAP_SKIP_GOEP_L2CAP_PSM_LEN +
                        PBAP_SKIP_SUPP_FEA_LEN;
                    SDP_TRACE_DEBUG("%s: All attributes requested"
                        " need to reduce length by %d", __func__,
                        blacklist_skip_len);
                }
            }
        }
        /* Allow space for attribute sequence type and length */
        p_seq_start = p_rsp;
        if (p_ccb->cont_info.last_attr_seq_desc_sent == FALSE)
        {
            /* See if there is enough room to include a new service in the current response */
            rem_len = max_list_len - (INT16) (p_rsp - &p_ccb->rsp_list[0]);
            if (rem_len < 3)
            {
                /* Not enough room. Update continuation info for next response */
                p_ccb->cont_info.next_attr_index = 0;
                p_ccb->cont_info.next_attr_start_id = attr_seq.attr_entry[0].start;
                break;
            }
            p_rsp += 3;
        }

        /* Get a list of handles that match the UUIDs given to us */
        for (xx = p_ccb->cont_info.next_attr_index; xx < attr_seq.num_attr; xx++)
        {
            p_attr = sdp_db_find_attr_in_rec (p_rec, attr_seq.attr_entry[xx].start, attr_seq.attr_entry[xx].end);

            if (p_attr)
            {
#if ((defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE)) || \
        (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE)))
                /* Check for UUID Remote Control and Remote BD address  */
                is_avrcp_fallback = sdp_fallback_avrcp_version (p_attr, p_ccb->device_address);
#if (defined(AVCT_BROWSE_INCLUDED)&&(AVCT_BROWSE_INCLUDED == TRUE))
                is_avrcp_browse_bit_reset = sdp_reset_avrcp_browsing_bit(
                            p_rec->attribute[1], p_attr, p_ccb->device_address);
#endif
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
                is_avrcp_ca_bit_reset = sdp_reset_avrcp_cover_art_bit(
                            p_rec->attribute[1], p_attr, p_ccb->device_address);
#endif
#endif
                is_hfp_fallback = sdp_change_hfp_version (p_attr, p_ccb->device_address);
                /* Check if attribute fits. Assume 3-byte value type/length */
                rem_len = max_list_len - (INT16) (p_rsp - &p_ccb->rsp_list[0]);

                /* just in case */
                if (rem_len <= 0)
                {
                    p_ccb->cont_info.next_attr_index = xx;
                    p_ccb->cont_info.next_attr_start_id = p_attr->id;
                    maxxed_out = TRUE;
                    break;
                }

                attr_len = sdpu_get_attrib_entry_len(p_attr);
                /* if there is a partial attribute pending to be sent */
                if (p_ccb->cont_info.attr_offset)
                {
                    p_rsp = sdpu_build_partial_attrib_entry (p_rsp, p_attr, rem_len,
                                                             &p_ccb->cont_info.attr_offset);

                    /* If the partial attrib could not been fully added yet */
                    if (p_ccb->cont_info.attr_offset != attr_len)
                    {
                        maxxed_out = TRUE;
                        break;
                    }
                    else /* If the partial attrib has been added in full by now */
                        p_ccb->cont_info.attr_offset = 0; /* reset attr_offset */
                }
                else if (rem_len < attr_len) /* Not enough space for attr... so add partially */
                {
                    if (attr_len >= SDP_MAX_ATTR_LEN)
                    {
                        SDP_TRACE_ERROR("SDP attr too big: max_list_len=%d,attr_len=%d", max_list_len, attr_len);
                        sdpu_build_n_send_error (p_ccb, trans_num, SDP_NO_RESOURCES, NULL);
                        return;
                    }

                    /* add the partial attribute if possible */
                    p_rsp = sdpu_build_partial_attrib_entry (p_rsp, p_attr, (UINT16)rem_len,
                                                             &p_ccb->cont_info.attr_offset);

                    p_ccb->cont_info.next_attr_index = xx;
                    p_ccb->cont_info.next_attr_start_id = p_attr->id;
                    maxxed_out = TRUE;
                    break;
                }
                else /* build the whole attribute */
                    p_rsp = sdpu_build_attrib_entry (p_rsp, p_attr);

                /* If doing a range, stick with this one till no more attributes found */
                if (attr_seq.attr_entry[xx].start != attr_seq.attr_entry[xx].end)
                {
                    /* Update for next time through */
                    attr_seq.attr_entry[xx].start = p_attr->id + 1;

                    xx--;
                }
                if (is_avrcp_fallback)
                {
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
                    /* Update AVRCP version back to 1.6 */
                    p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
#else
#if (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE))
                    /* Update AVRCP version back to 1.5 */
                    p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x05;
#endif
#endif
                    is_avrcp_fallback = FALSE;
                }
                if (is_avrcp_browse_bit_reset)
                {
                    /* Restore Browsing bit */
                    SDP_TRACE_ERROR("Restore Browsing bit");
                    p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION]
                                            |= AVRCP_BROWSE_SUPPORT_BITMASK;
                    is_avrcp_browse_bit_reset = FALSE;
                }
                if (is_hfp_fallback)
                {
                    SDP_TRACE_ERROR("Restore HFP version to 1.6");
                    /* Update HFP version back to 1.6 */
                    p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
                    is_hfp_fallback = FALSE;
                }
                if (is_avrcp_ca_bit_reset)
                {
                    /* Restore Cover Art bit */
                    SDP_TRACE_ERROR("Restore Cover Art bit");
                    p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION - 1]
                                            |= AVRCP_CA_SUPPORT_BITMASK;
                    is_avrcp_ca_bit_reset = FALSE;
                }
            }
        }
        if (is_avrcp_fallback)
        {
#if (defined(SDP_AVRCP_1_6) && (SDP_AVRCP_1_6 == TRUE))
            /* Update AVRCP version back to 1.6 */
            p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
#else
#if (defined(SDP_AVRCP_1_5) && (SDP_AVRCP_1_5 == TRUE))
            /* Update AVRCP version back to 1.5 */
            p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x05;
#endif
#endif
            is_avrcp_fallback = FALSE;
        }
        if (is_avrcp_browse_bit_reset)
        {
            /* Restore Browsing bit */
            SDP_TRACE_ERROR("Restore Browsing bit");
            p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION]
                                    |= AVRCP_BROWSE_SUPPORT_BITMASK;
            is_avrcp_browse_bit_reset = FALSE;
        }
        if (is_hfp_fallback)
        {
            SDP_TRACE_ERROR("Restore HFP version to 1.6");
            /* Update HFP version back to 1.6 */
            p_attr->value_ptr[PROFILE_VERSION_POSITION] = 0x06;
            is_hfp_fallback = FALSE;
        }
        if (is_avrcp_ca_bit_reset)
        {
            /* Restore Cover Art bit */
            SDP_TRACE_ERROR("Restore Cover Art bit");
            p_attr->value_ptr[AVRCP_SUPPORTED_FEATURES_POSITION - 1]
                                    |= AVRCP_CA_SUPPORT_BITMASK;
            is_avrcp_ca_bit_reset = FALSE;
        }

        /* Go back and put the type and length into the buffer */
        if (p_ccb->cont_info.last_attr_seq_desc_sent == FALSE)
        {
            seq_len = sdpu_get_attrib_seq_len(p_rec, &attr_seq_sav);
            if (seq_len != 0)
            {
                if (p_seq_start)
                {
                    UINT8_TO_BE_STREAM  (p_seq_start, (DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_WORD);
                    UINT16_TO_BE_STREAM (p_seq_start, seq_len);
                }
                else
                {
                    SDP_TRACE_DEBUG("SDP service and attribute rsp: Attribute sequence p_seq_start is NULL");
                }

                if (maxxed_out)
                    p_ccb->cont_info.last_attr_seq_desc_sent = TRUE;
            }
            else
                p_rsp = p_seq_start;
        }

        if (maxxed_out)
            break;

        /* Restore the attr_seq to look for in the next sdp record */
        memcpy(&attr_seq, &attr_seq_sav, sizeof(tSDP_ATTR_SEQ)) ;

        /* Reset the next attr index */
        p_ccb->cont_info.next_attr_index = 0;
        /* restore the record pointer.*/
        p_rec = p_prev_rec;
        p_ccb->cont_info.prev_sdp_rec = p_rec;
        p_ccb->cont_info.last_attr_seq_desc_sent = FALSE;
    }

    /* response length */
    len_to_send = (UINT16) (p_rsp - &p_ccb->rsp_list[0]);
    cont_offset = 0;

    // The current SDP server design has a critical flaw where it can run into an infinite
    // request/response loop with the client. Here's the scenario:
    // - client makes SDP request
    // - server returns the first fragment of the response with a continuation token
    // - an SDP record is deleted from the server
    // - client issues another request with previous continuation token
    // - server has nothing to send back because the record is unavailable but in the
    //   first fragment, it had specified more response bytes than are now available
    // - server sends back no additional response bytes and returns the same continuation token
    // - client issues another request with the continuation token, and the process repeats
    //
    // We work around this design flaw here by checking if we will make forward progress
    // (i.e. we will send > 0 response bytes) on a continued request. If not, we must have
    // run into the above situation and we tell the peer an error occurred.
    //
    // TODO(sharvil): rewrite SDP server.
    if (is_cont && len_to_send == 0) {
      sdpu_build_n_send_error(p_ccb, trans_num, SDP_INVALID_CONT_STATE, NULL);
      return;
    }

    /* If first response, insert sequence header */
    if (!is_cont)
    {
        /* Get the total list length for requested uid and attribute sequence */
        p_ccb->list_len = sdpu_get_list_len(&uid_seq, &attr_seq_sav) + 3;
        if (blacklist_skip_len &&
            p_ccb->list_len > blacklist_skip_len) {
            p_ccb->list_len -= blacklist_skip_len;
            SDP_TRACE_DEBUG("%s: reducing list_len by %d for blacklisted device",
                __func__, blacklist_skip_len);
            blacklist_skip_len = 0;
        }
        /* Put in the sequence header (2 or 3 bytes) */
        if (p_ccb->list_len > 255)
        {
            p_ccb->rsp_list[0] = (UINT8) ((DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_WORD);
            p_ccb->rsp_list[1] = (UINT8) ((p_ccb->list_len - 3) >> 8);
            p_ccb->rsp_list[2] = (UINT8) (p_ccb->list_len - 3);
        }
        else
        {
            cont_offset = 1;

            p_ccb->rsp_list[1] = (UINT8) ((DATA_ELE_SEQ_DESC_TYPE << 3) | SIZE_IN_NEXT_BYTE);
            p_ccb->rsp_list[2] = (UINT8) (p_ccb->list_len - 3);

            p_ccb->list_len--;
            len_to_send--;
        }
    }

    /* Get a buffer to use to build the response */
    p_buf = (BT_HDR *)osi_malloc(SDP_DATA_BUF_SIZE);
    p_buf->offset = L2CAP_MIN_OFFSET;
    p_rsp = p_rsp_start = (UINT8 *)(p_buf + 1) + L2CAP_MIN_OFFSET;

    /* Start building a rsponse */
    UINT8_TO_BE_STREAM  (p_rsp, SDP_PDU_SERVICE_SEARCH_ATTR_RSP);
    UINT16_TO_BE_STREAM (p_rsp, trans_num);

    /* Skip the parameter length, add it when we know the length */
    p_rsp_param_len = p_rsp;
    p_rsp += 2;

    /* Stream the list length to send */
    UINT16_TO_BE_STREAM (p_rsp, len_to_send);

    /* copy from rsp_list to the actual buffer to be sent */
    memcpy (p_rsp, &p_ccb->rsp_list[cont_offset], len_to_send);
    p_rsp += len_to_send;

    p_ccb->cont_offset += len_to_send;

    if (blacklist_skip_len &&
        p_ccb->list_len > blacklist_skip_len) {
        p_ccb->list_len -= blacklist_skip_len;
        SDP_TRACE_DEBUG("%s: reducing list_len by %d for blacklisted device",
            __func__, blacklist_skip_len);
        blacklist_skip_len = 0;
    }

    /* If anything left to send, continuation needed */
    if (p_ccb->cont_offset < p_ccb->list_len)
    {
        is_cont = TRUE;

        UINT8_TO_BE_STREAM  (p_rsp, SDP_CONTINUATION_LEN);
        UINT16_TO_BE_STREAM (p_rsp, p_ccb->cont_offset);
    }
    else
        UINT8_TO_BE_STREAM (p_rsp, 0);

    /* Go back and put the parameter length into the buffer */
    rsp_param_len = p_rsp - p_rsp_param_len - 2;
    UINT16_TO_BE_STREAM (p_rsp_param_len, rsp_param_len);

    /* Set the length of the SDP data in the buffer */
    p_buf->len = p_rsp - p_rsp_start;


    /* Send the buffer through L2CAP */
    L2CA_DataWrite (p_ccb->connection_id, p_buf);
}

/*************************************************************************************
**
** Function        is_pbap_record_blacklisted
**
** Description     Checks if given PBAP record is for PBAP PSE and blacklisted
**
** Returns         BOOLEAN
**
***************************************************************************************/
static BOOLEAN is_pbap_record_blacklisted (tSDP_ATTRIBUTE attr,
                                      BD_ADDR remote_address)
{
    if ((attr.id == ATTR_ID_SERVICE_CLASS_ID_LIST) &&
        (((attr.value_ptr[1] << 8) | (attr.value_ptr[2])) ==
        UUID_SERVCLASS_PBAP_PSE))
    {
        bt_bdaddr_t remote_bdaddr;
        bdcpy(remote_bdaddr.address, remote_address);

        bt_property_t prop_name;
        bt_bdname_t bdname;

        memset(&bdname, 0, sizeof(bt_bdname_t));
        BTIF_STORAGE_FILL_PROPERTY(&prop_name, BT_PROPERTY_BDNAME,
                               sizeof(bt_bdname_t), &bdname);
        if (btif_storage_get_remote_device_property(&remote_bdaddr,
                                              &prop_name) != BT_STATUS_SUCCESS) {
            SDP_TRACE_DEBUG("%s: BT_PROPERTY_BDNAME failed", __func__);
        }
        if (interop_match_addr(INTEROP_ADV_PBAP_VER_1_1, &remote_bdaddr) ||
           (strlen((const char *)bdname.name) != 0 &&
            interop_match_name(INTEROP_ADV_PBAP_VER_1_1,
            (const char *)bdname.name))) {
            SDP_TRACE_DEBUG("%s: device is blacklisted for pbap version downgrade", __func__);
            return TRUE;
        }
    }

    return FALSE;
}

/*************************************************************************************
**
** Function        sdp_update_pbap_record_if_blacklisted
**
** Description     updates pbap record after checking if blacklisted
**
** Returns         the address of updated record
**
***************************************************************************************/
static tSDP_RECORD *sdp_update_pbap_record_if_blacklisted(tSDP_RECORD *p_rec,
                                      BD_ADDR remote_address)
{
    static tSDP_RECORD pbap_temp_sdp_rec;
    static BOOLEAN is_blacklisted_rec_created = FALSE;

    /* Check if the given SDP record is blacklisted and requires updatiion */
    if (is_pbap_record_blacklisted(p_rec->attribute[1], remote_address)) {
        if (is_blacklisted_rec_created)
            return &pbap_temp_sdp_rec;

        bool status = TRUE;
        int xx;
        UINT8 supported_repositories = 0x03;
        UINT16 legacy_version = 0x0101;
        memset(&pbap_temp_sdp_rec, 0, sizeof(tSDP_RECORD));

        tSDP_ATTRIBUTE  *p_attr = &p_rec->attribute[0];

        /* Copying contents of the PBAP PSE record to a temporary record */
        for (xx = 0; xx < p_rec->num_attributes; xx++, p_attr++)
            SDP_AddAttributetoRecord (&pbap_temp_sdp_rec, p_attr->id,
            p_attr->type, p_attr->len, p_attr->value_ptr);

        status &= SDP_DeleteAttributefromRecord (&pbap_temp_sdp_rec,
            ATTR_ID_PBAP_SUPPORTED_FEATURES);
        status &= SDP_DeleteAttributefromRecord (&pbap_temp_sdp_rec,
            ATTR_ID_GOEP_L2CAP_PSM);
        status &= SDP_AddAttributetoRecord (&pbap_temp_sdp_rec,
            ATTR_ID_SUPPORTED_REPOSITORIES, UINT_DESC_TYPE, (UINT32)1,
            (UINT8*)&supported_repositories);
        status &= SDP_AddProfileDescriptorListtoRecord(&pbap_temp_sdp_rec,
            UUID_SERVCLASS_PHONE_ACCESS, legacy_version);
        if (!status) {
            SDP_TRACE_ERROR("%s() FAILED", __func__);
            return p_rec;
        }
        is_blacklisted_rec_created = TRUE;
        return &pbap_temp_sdp_rec;
    }
    return p_rec;
}

#endif  /* SDP_SERVER_ENABLED == TRUE */
