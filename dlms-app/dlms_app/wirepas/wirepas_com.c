/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#include "wirepas_com.h"
#include "api.h"
#include "shared_data.h"
#include "opt_passthrough.h"
#include "nic_server.h"
#include "common.h"
#include "nic_status.h"
#include "stack_state.h"

#define DEBUG_LOG_MODULE_NAME "WP_COM  "
#define DEBUG_LOG_MAX_LEVEL LVL_INFO
#include "debug_log.h"

/** Endpoints for incoming DLMS traffic*/
#define DLMS_LEGACY_EP   1 // Legacy endpoint
#define ON_DEMAND_SRC_EP 2

#define ON_DEMAND_MIN_EP 32
#define ON_DEMAND_MAX_EP 63

#define NOTIFICATION_SRC_EP 1

// Global variables
static wirepas_com_first_valid_route_cb_f m_first_valid_route_cb;
static wirepas_com_status_change_cb_f m_status_change_cb;
static bool m_valid_route;
static app_addr_t m_current_sink = 0;

static uint8_t m_last_request_dst_ep = 0;
static uint8_t m_last_request_src_ep = 0;

static app_lib_data_receive_res_e _unicastDLMSDataReceivedCb(const shared_data_item_t * item,
                                                    const app_lib_data_received_t * data);

static app_lib_data_receive_res_e _unicastDLMSDataReceivedCbLegacy(const shared_data_item_t * item,
                                                    const app_lib_data_received_t * data);

static shared_data_item_t m_dlms_packets_filter =
{
        .cb = _unicastDLMSDataReceivedCb,
        .filter = {
                .mode = SHARED_DATA_NET_MODE_UNICAST,
                .src_endpoint = ON_DEMAND_SRC_EP,
                .dest_endpoint = SHARED_DATA_UNUSED_ENDPOINT,
                .multicast_cb = NULL
        }
};

static shared_data_item_t m_dlms_packets_filter_legacy =
{
        .cb = _unicastDLMSDataReceivedCbLegacy,
        .filter = {
                .mode = SHARED_DATA_NET_MODE_UNICAST,
                .src_endpoint = DLMS_LEGACY_EP,
                .dest_endpoint = DLMS_LEGACY_EP,
                .multicast_cb = NULL
        }
};

/**
 * Big Endian to Little Endian conversion for uint16_t
*/
static inline uint16_t swap_BE_to_LE(uint16_t val)
{
    return (val>>8) | (val<<8);
}

static app_lib_data_receive_res_e _unicastDLMSDataReceivedCbLegacy(const shared_data_item_t * item,
                                                    const app_lib_data_received_t * data)
{
    // Add a warning but still accept keep going
    LOGW("Using old EP for on-demand or Nic server. 2 is the new one");
    // Call the normal unicast Cb
    return _unicastDLMSDataReceivedCb(item, data);
}

static app_lib_data_receive_res_e _unicastDLMSDataReceivedCb(const shared_data_item_t * item,
                                                    const app_lib_data_received_t * data)
{
    // Get the header of WRAPPER request
    const wrapper_header_t * wrapper_header_p;
    wrapper_header_p = (wrapper_header_t *) data->bytes;
    mcm_aa_e aa_type;
    bool res;

    // If we are here, source endpoint is either DLMS_LEGACY_EP or ON_DEMAND_SRC_EP
    // No need for additionnal check on scr ep

    // Destination endpoint has to be in the [ON_DEMAND_MIN_EP; ON_DEMAND_MAX_EP] range
    // if src is ON_DEMAND_SRC_EP, otherwise we are in legacy case 1 -> 1
    if (data->src_endpoint == ON_DEMAND_SRC_EP)
    {
        if (data->dest_endpoint < ON_DEMAND_MIN_EP || data->dest_endpoint > ON_DEMAND_MAX_EP)
        {
            LOGE("Wrong EP: %d", data->dest_endpoint);
            return APP_LIB_DATA_RECEIVE_RES_HANDLED;
        }
    }

    wrapper_header_t header = {
        .destination = swap_BE_to_LE(wrapper_header_p->destination),
        .source = swap_BE_to_LE(wrapper_header_p->source),
        .version = swap_BE_to_LE(wrapper_header_p->version),
        .length = swap_BE_to_LE(wrapper_header_p->length)
    };

    LOGD("Request from 0x%x to 0x%x (version=%d length=%d)",
                    header.source,
                    header.destination,
                    header.version,
                    header.length);

    if (data->num_bytes < sizeof(wrapper_header_t))
    {
        LOGE("Not enough data");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    if (header.version != 1)
    {
        LOGE("Wrong wrapper version");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    // Check that client address is valid
    if (! Meter_Connection_Management_addr_to_aa(header.source, &aa_type))
    {
        LOGE("Wrong client address 0x%x", header.source);
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    // Check who is destination (NIC server or meter Server)
    if (header.destination == NIC_SERVER_ADDRESS)
    {
        res = Nic_Server_handle_message(data->bytes, data->num_bytes,
                                  header.destination, aa_type, data->delay_hp);
    }
    else if (header.destination == METER_SERVER_ADDRESS)
    {
        res = Opt_Passthrough_handle_passthrough_message(data->bytes, data->num_bytes,
                                                   aa_type);
    }
    else
    {
        LOGE("Unsupported server address");
        return APP_LIB_DATA_RECEIVE_RES_HANDLED;
    }

    if (res)
    {
        // Request was accepted, so a reply should be issued
        // Update our Endpoints to send the respone on correct EP
        m_last_request_dst_ep = data->dest_endpoint;
        m_last_request_src_ep = data->src_endpoint;
    }

    return APP_LIB_DATA_RECEIVE_RES_HANDLED;
}

static void on_stack_event_cb(app_lib_stack_event_e event, void * param)
{
    bool valid_route = m_valid_route;

    if (! m_status_change_cb)
    {
        return;
    }

    switch (event)
    {
        // Stack has stopped. Param = NULL
        case APP_LIB_STATE_STACK_EVENT_STACK_STOPPED:
            LOGI("STACK_STOPPED");
            valid_route = false;
            break;

        // Stack has started. Param = NULL
        case APP_LIB_STATE_STACK_EVENT_STACK_STARTED:
            valid_route = false;
            break;

        // A scratchpad transfer (TX or RX) has started.
        // It will prevent app from being scheduled for up to 30s. Param = NULL
        case APP_LIB_STATE_STACK_EVENT_SCRAT_XFER_STARTED:
            LOGI("SCRAT_XFER_STARTED");
            valid_route = false;
            break;

        // Scratchpad transfer is finished and app will be scheduled normaly.
        // Param = NULL
        case APP_LIB_STATE_STACK_EVENT_SCRAT_XFER_STOPPED:
            LOGI("SCRAT_XFER_STOPPED");
            valid_route = false;
            break;

        // Route has changed. Param = pointer to app_lib_state_route_info_t
        case APP_LIB_STATE_STACK_EVENT_ROUTE_CHANGED:
            {
                const app_lib_state_route_info_t * info_p = (app_lib_state_route_info_t *) param;

                if (info_p->state == APP_LIB_STATE_ROUTE_STATE_VALID)
                {
                    LOGI("Valid route to sink 0x%X", info_p->sink);
                    valid_route = true;
                    if (m_first_valid_route_cb)
                    {
                        m_first_valid_route_cb();
                        m_first_valid_route_cb = NULL;
                    }

                    if (m_current_sink != info_p->sink)
                    {
                        if (m_current_sink != 0)
                        {
                            // Nic status is sent only if we were attached to another sink previously
                            // If it is not the case, we will send it very soon.
                            Nic_status_generate_and_send_notification(STATUS_REASON_SINK_CHANGE);
                        }
                        m_current_sink = info_p->sink;
                    }
                }
                else
                {
                    LOGI("No route");
                    valid_route = false;
                }
            }
            break;
        default:
            break;
            // Unknown event, nothing to do
     }

     if (m_valid_route != valid_route)
     {
        m_valid_route = valid_route;
        m_status_change_cb(valid_route);
     }
}

void Wirepas_com_init(wirepas_com_first_valid_route_cb_f cb, bool limited_mode)
{
    if (! cb)
    {
        LOGE("Valid route callback has to be set");
        // TODO handle error
        return;
    }
    // Init variables
    m_first_valid_route_cb = cb;

    // Subscribe to stack events
    Stack_State_addEventCb(on_stack_event_cb, STACK_STATE_ALL_EVENTS_BITFIELDS);

    // Start the stack
    Stack_State_startStack();

    if (! limited_mode)
    {
        Shared_Data_addDataReceivedCb(&m_dlms_packets_filter);
        Shared_Data_addDataReceivedCb(&m_dlms_packets_filter_legacy);

    }
    LOGI("Client started!");
}

bool Wirepas_com_send_message(const uint8_t * data, uint16_t len,
                              app_lib_data_data_sent_cb_f sent_cb,
                              Wirepas_com_traffic_type_e type)
{
    app_lib_data_send_res_e res;

    uint16_t id = 0;
    uint8_t flag = APP_LIB_DATA_SEND_FLAG_NONE;
    uint8_t destination_ep;
    uint8_t source_ep;

    // we use tracking cb lowest part of address as tracking id to ensure that each client only send one packet at a time
    if (sent_cb != NULL)
    {
        id = (uint32_t) (sent_cb) & 0xFFFF;
        flag = APP_LIB_DATA_SEND_FLAG_TRACK;
    }

    if (type >= WC_TYPE_PUSH_FIRST && type <= WC_TYPE_PUSH_LAST)
    {
        // Type value is mapped to destination EP to use
        destination_ep = type;
        source_ep = NOTIFICATION_SRC_EP;
    }
    else if (type == WC_TYPE_ON_DEMAND)
    {
        if (m_last_request_dst_ep == 0)
        {
            // No previous request, set it to default one
            destination_ep = DLMS_LEGACY_EP;
            LOGW("No dst endpoint for on demand reply");
        }
        else
        {
            destination_ep = m_last_request_dst_ep;
        }

        if (m_last_request_src_ep == 0)
        {
            source_ep = DLMS_LEGACY_EP;
            LOGW("No src endpoint for on demand reply");
        }
        else
        {
            source_ep = m_last_request_src_ep;
        }
    }
    else
    {
        LOGE("Unknown type: %d", type);
        return false;
    }

    app_lib_data_to_send_t data_to_send = {
            .bytes = (const uint8_t*)data,
            .num_bytes = len,
            .dest_address = APP_ADDR_ANYSINK,
            .src_endpoint = source_ep,
            .dest_endpoint = destination_ep,
            .qos = APP_LIB_DATA_QOS_HIGH,
            .flags = flag,
            .tracking_id = id,
    };

    res = Shared_Data_sendData(&data_to_send,
                               sent_cb);

    if (res == APP_LIB_DATA_SEND_RES_INVALID_TRACKING_ID)
    {
        LOGE("Previous message was still in queue");
    }

    return res == APP_LIB_DATA_SEND_RES_SUCCESS;
}

void Wirepas_com_subscribeStatusChangeCb(wirepas_com_status_change_cb_f cb)
{
    m_status_change_cb = cb;
}
