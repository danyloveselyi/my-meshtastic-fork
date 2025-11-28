/**
 * @file NodeHotData.h
 * @brief Hot data structure for frequently updated routing information
 * 
 * This module defines the hot data structure that contains only frequently
 * updated fields that are NEVER written to flash. This prevents flash wear
 * from constant updates (e.g., last_heard updates 100-1000x per minute).
 * 
 * Hot data (RAM only, ~33 bytes):
 * - NodeNum num
 * - uint32_t last_heard (NEVER written to flash - updates every packet)
 * - uint8_t channel (may change when node switches channels)
 * - float snr (NEVER written to flash - updates every packet)
 * - uint8_t hops_away (NEVER written to flash - updates frequently)
 * - bool via_mqtt (updates every packet)
 * - uint8_t next_hop (NEVER written to flash - updates during routing)
 * - Flags (is_favorite, is_ignored, is_key_manually_verified)
 * 
 * Cold data (stored in flash, loaded on-demand):
 * - meshtastic_User user (name, macaddr, public_key)
 * - meshtastic_Position position (lat, lon, altitude)
 * - meshtastic_DeviceMetrics device_metrics
 * - Flags (is_favorite, is_ignored, is_key_manually_verified)
 */

#pragma once

#include "configuration.h"
#include "../../../src/mesh/NodeDB.h"
#include <cstdint>

#ifdef USE_EXTENDED_FS_FOR_NODEDB

/**
 * @brief Hot data structure for routing (RAM only, never written to flash)
 * 
 * This structure contains only frequently updated fields that are critical
 * for routing but change too often to write to flash.
 */
struct NodeHotData {
    uint32_t num;              // Node number
    uint32_t last_heard;       // Last heard timestamp (NEVER written to flash - updates every packet)
    uint8_t channel;           // Channel index (may change when node switches channels)
    float snr;                 // Signal-to-noise ratio (NEVER written to flash - updates every packet)
    uint8_t hops_away;         // Number of hops away (NEVER written to flash - updates frequently)
    bool has_hops_away;        // Whether hops_away is valid
    bool via_mqtt;             // Received via MQTT (updates every packet)
    uint8_t next_hop;          // Next hop node for routing (NEVER written to flash - updates during routing)
    bool is_favorite;          // Favorite flag (rarely changes)
    bool is_ignored;           // Ignored flag (rarely changes)
    bool is_key_manually_verified; // Key verification flag (from bitfield LSB 0, rarely changes)
    
    /**
     * @brief Create hot data from NodeInfoLite
     */
    static NodeHotData fromNodeInfoLite(const meshtastic_NodeInfoLite* node)
    {
        NodeHotData hot;
        hot.num = node->num;
        hot.last_heard = node->last_heard;
        hot.channel = node->channel;
        hot.snr = node->snr;
        hot.hops_away = node->has_hops_away ? node->hops_away : 0;
        hot.has_hops_away = node->has_hops_away;
        hot.via_mqtt = node->via_mqtt;
        hot.next_hop = node->next_hop;  // Routing information (updates during routing)
        hot.is_favorite = node->is_favorite;
        hot.is_ignored = node->is_ignored;
        // Extract is_key_manually_verified from bitfield (LSB 0)
        hot.is_key_manually_verified = (node->bitfield & 0x01) != 0;
        return hot;
    }
    
    /**
     * @brief Update NodeInfoLite with hot data
     */
    void updateNodeInfoLite(meshtastic_NodeInfoLite* node) const
    {
        node->num = num;
        node->last_heard = last_heard;
        node->channel = channel;
        node->snr = snr;
        if (has_hops_away) {
            node->has_hops_away = true;
            node->hops_away = hops_away;
        }
        node->via_mqtt = via_mqtt;
        node->next_hop = next_hop;  // Routing information
        node->is_favorite = is_favorite;
        node->is_ignored = is_ignored;
        // Update bitfield (LSB 0 = is_key_manually_verified)
        if (is_key_manually_verified) {
            node->bitfield |= 0x01;
        } else {
            node->bitfield &= ~0x01;
        }
    }
};

/**
 * @brief Cold data structure (stored in flash, rarely updated)
 * 
 * This structure contains fields that change infrequently and can be
 * safely stored in flash. These are loaded on-demand when full node
 * information is needed.
 */
struct NodeColdData {
    bool has_user;
    meshtastic_UserLite user;
    bool has_position;
    meshtastic_PositionLite position;
    bool has_device_metrics;
    meshtastic_DeviceMetrics device_metrics;
    
    /**
     * @brief Create cold data from NodeInfoLite
     */
    static NodeColdData fromNodeInfoLite(const meshtastic_NodeInfoLite* node)
    {
        NodeColdData cold;
        cold.has_user = node->has_user;
        if (cold.has_user) {
            cold.user = node->user;  // NodeInfoLite uses UserLite
        }
        cold.has_position = node->has_position;
        if (cold.has_position) {
            cold.position = node->position;  // NodeInfoLite uses PositionLite
        }
        cold.has_device_metrics = node->has_device_metrics;
        if (cold.has_device_metrics) {
            cold.device_metrics = node->device_metrics;
        }
        return cold;
    }
    
    /**
     * @brief Update NodeInfoLite with cold data
     */
    void updateNodeInfoLite(meshtastic_NodeInfoLite* node) const
    {
        node->has_user = has_user;
        if (has_user) {
            node->user = user;  // NodeInfoLite uses UserLite
        }
        node->has_position = has_position;
        if (has_position) {
            node->position = position;  // NodeInfoLite uses PositionLite
        }
        node->has_device_metrics = has_device_metrics;
        if (has_device_metrics) {
            node->device_metrics = device_metrics;
        }
    }
};

/**
 * @brief Merge hot and cold data into full NodeInfoLite
 */
inline void mergeHotColdData(meshtastic_NodeInfoLite* node, 
                             const NodeHotData* hot, 
                             const NodeColdData* cold)
{
    if (hot) {
        hot->updateNodeInfoLite(node);
    }
    if (cold) {
        cold->updateNodeInfoLite(node);
    }
}

#endif // USE_EXTENDED_FS_FOR_NODEDB

