/*
 * TELEMETRY MEMORY OPTIMIZATION STRATEGY
 * 
 * This file implements memory-efficient telemetry using existing protobuf structures
 * instead of creating custom protobuf definitions. This saves significant Flash memory
 * by reusing generated protobuf code.
 * 
 * PROTOBUF REUSE EXAMPLES:
 * 1. meshtastic_EnvironmentMetrics → Memory/node statistics (DeviceTelemetry)
 * 2. meshtastic_PowerMetrics → Power sensor data (PowerTelemetry) 
 * 3. meshtastic_HealthMetrics → Health sensor data (HealthTelemetry)
 * 4. meshtastic_LocalStats → Local mesh statistics (DeviceTelemetry)
 * 
 * This approach maintains protocol compatibility while optimizing memory usage
 * for resource-constrained devices like nRF52840.
 */

#include "DeviceTelemetry.h"
#include "../mesh/generated/meshtastic/telemetry.pb.h"  // Existing protobuf definitions - no custom protobufs needed
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"
#include "mesh/mesh-pb-constants.h"
#include "memGet.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#include <meshUtils.h>

#define MAGIC_USB_BATTERY_LEVEL 101

int32_t DeviceTelemetryModule::runOnce()
{
    refreshUptime();
    bool isImpoliteRole =
        IS_ONE_OF(config.device.role, meshtastic_Config_DeviceConfig_Role_SENSOR, meshtastic_Config_DeviceConfig_Role_ROUTER);
    if (((lastSentToMesh == 0) ||
         ((uptimeLastMs - lastSentToMesh) >=
          Default::getConfiguredOrDefaultMsScaled(moduleConfig.telemetry.device_update_interval,
                                                  default_telemetry_broadcast_interval_secs, numOnlineNodes))) &&
        airTime->isTxAllowedChannelUtil(!isImpoliteRole) && airTime->isTxAllowedAirUtil() &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_REPEATER &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN) {
        sendTelemetry();
        lastSentToMesh = uptimeLastMs;
    } else if (service->isToPhoneQueueEmpty()) {
        // Just send to phone when it's not our time to send to mesh yet
        // Only send while queue is empty (phone assumed connected)
        sendTelemetry(NODENUM_BROADCAST, true);
        if (lastSentStatsToPhone == 0 || (uptimeLastMs - lastSentStatsToPhone) >= sendStatsToPhoneIntervalMs) {
            sendLocalStatsToPhone();
            lastSentStatsToPhone = uptimeLastMs;
        }
    }
    return sendToPhoneIntervalMs;
}

bool DeviceTelemetryModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *t)
{
    // Don't worry about storing telemetry in NodeDB if we're a repeater
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_REPEATER)
        return false;

    if (t->which_variant == meshtastic_Telemetry_device_metrics_tag) {
#ifdef DEBUG_PORT
        const char *sender = getSenderShortName(mp);

        LOG_INFO("(Received from %s): air_util_tx=%f, channel_utilization=%f, battery_level=%i, voltage=%f", sender,
                 t->variant.device_metrics.air_util_tx, t->variant.device_metrics.channel_utilization,
                 t->variant.device_metrics.battery_level, t->variant.device_metrics.voltage);
#endif
        nodeDB->updateTelemetry(getFrom(&mp), *t, RX_SRC_RADIO);
    }
    return false; // Let others look at this message also if they want
}

meshtastic_MeshPacket *DeviceTelemetryModule::allocReply()
{
    if (currentRequest) {
        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch;
        meshtastic_Telemetry *decoded = NULL;
        memset(&scratch, 0, sizeof(scratch));
        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            decoded = &scratch;
        } else {
            LOG_ERROR("Error decoding DeviceTelemetry module!");
            return NULL;
        }
        // Check for a request for device metrics
        if (decoded->which_variant == meshtastic_Telemetry_device_metrics_tag) {
            LOG_INFO("Device telemetry reply to request");
            return allocDataProtobuf(getDeviceTelemetry());
        } else if (decoded->which_variant == meshtastic_Telemetry_local_stats_tag) {
            LOG_INFO("Device telemetry reply w/ LocalStats to request");
            return allocDataProtobuf(getLocalStatsTelemetry());
        }
    }
    return NULL;
}

meshtastic_Telemetry DeviceTelemetryModule::getDeviceTelemetry()
{
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    t.which_variant = meshtastic_Telemetry_device_metrics_tag;
    t.time = getTime();
    t.variant.device_metrics = meshtastic_DeviceMetrics_init_zero;
    t.variant.device_metrics.has_air_util_tx = true;
    t.variant.device_metrics.has_battery_level = true;
    t.variant.device_metrics.has_channel_utilization = true;
    t.variant.device_metrics.has_voltage = true;
    t.variant.device_metrics.has_uptime_seconds = true;

    t.variant.device_metrics.air_util_tx = airTime->utilizationTXPercent();
    t.variant.device_metrics.battery_level = (!powerStatus->getHasBattery() || powerStatus->getIsCharging())
                                                 ? MAGIC_USB_BATTERY_LEVEL
                                                 : powerStatus->getBatteryChargePercent();
    t.variant.device_metrics.channel_utilization = airTime->channelUtilizationPercent();
    t.variant.device_metrics.voltage = powerStatus->getBatteryVoltageMv() / 1000.0;
    t.variant.device_metrics.uptime_seconds = getUptimeSeconds();

    return t;
}

/*
 * Memory-efficient telemetry implementation using existing protobuf structures
 * 
 * OPTIMIZATION STRATEGY: Instead of creating custom protobuf messages for memory stats,
 * this function reuses the existing meshtastic_EnvironmentMetrics protobuf structure.
 * This saves Flash memory by avoiding additional protobuf definitions and generated code.
 * 
 * FIELD MAPPING (Environment → Memory Stats):
 * - gas_resistance     → Flash total (KB) 
 * - relative_humidity  → Flash free (KB)
 * - iaq               → Heap total (KB)
 * - lux               → Heap free (KB)  
 * - white_lux         → Online mesh nodes count
 * - barometric_pressure → Total mesh nodes count
 *
 * This approach maintains client compatibility while providing memory monitoring
 * functionality without additional protobuf overhead.
 */
meshtastic_Telemetry DeviceTelemetryModule::getMemoryStatsAsEnvironmentTelemetry()
{
    // Reuse existing meshtastic_Telemetry protobuf - no custom protobuf needed for memory optimization
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    t.which_variant = meshtastic_Telemetry_environment_metrics_tag;  // Use environment variant for memory data
    t.time = getTime();
    t.variant.environment_metrics = meshtastic_EnvironmentMetrics_init_zero;

    // FLASH MEMORY STATISTICS - repurpose gas/humidity fields for flash info
    // gas_resistance field → Flash total size in KB
    t.variant.environment_metrics.has_gas_resistance = true;
    t.variant.environment_metrics.gas_resistance = memGet.getFlashTotal() / 1024.0f; // Flash total in KB

    // relative_humidity field → Flash free space in KB  
    t.variant.environment_metrics.has_relative_humidity = true;
    t.variant.environment_metrics.relative_humidity = memGet.getFlashFree() / 1024.0f; // Flash free in KB

    // HEAP MEMORY STATISTICS - repurpose air quality fields for heap info
    // iaq field → Heap total size in KB (as integer for precision)
    t.variant.environment_metrics.has_iaq = true;
    t.variant.environment_metrics.iaq = (uint32_t)(memGet.getHeapSize() / 1024); // Heap total in KB (as integer)

    // lux field → Heap free space in KB
    t.variant.environment_metrics.has_lux = true;
    t.variant.environment_metrics.lux = memGet.getFreeHeap() / 1024.0f; // Heap free in KB

    // MESH NETWORK STATISTICS - repurpose light sensor fields for node counts
    // white_lux field → Active/online mesh nodes count
    t.variant.environment_metrics.has_white_lux = true;
    t.variant.environment_metrics.white_lux = nodeDB->getNumOnlineMeshNodes(); // Number of online mesh nodes

    // barometric_pressure field → Total mesh nodes in database (including offline)
    t.variant.environment_metrics.has_barometric_pressure = true;
    t.variant.environment_metrics.barometric_pressure = nodeDB->getNumMeshNodes(); // Total nodes in database

    // current field → Maximum nodes limit configured for this device
    t.variant.environment_metrics.has_current = true;
    t.variant.environment_metrics.current = MAX_NUM_NODES; // Maximum nodes configured

    // Debug log showing field mapping: Environment protobuf fields → Actual memory/node data
    LOG_INFO("MemoryStats as Environment: Flash(gas_resistance/relative_humidity)=%.1f/%.1fKB, Heap(iaq/lux)=%u/%.1fKB, Nodes(white_lux/barometric_pressure/current)=%.0f/%.1f/%.1f",
             t.variant.environment_metrics.gas_resistance, t.variant.environment_metrics.relative_humidity,
             t.variant.environment_metrics.iaq, t.variant.environment_metrics.lux,
             t.variant.environment_metrics.white_lux, t.variant.environment_metrics.barometric_pressure, t.variant.environment_metrics.current);
    
    // Raw memory values for debugging - actual system calls before field mapping
    LOG_INFO("Raw memory values: FlashTotal=%u, FlashUsed=%u, FlashFree=%u, HeapTotal=%u, HeapFree=%u",
             memGet.getFlashTotal(), memGet.getFlashUsed(), memGet.getFlashFree(),
             memGet.getHeapSize(), memGet.getFreeHeap());
    return t;
}

meshtastic_Telemetry DeviceTelemetryModule::getLocalStatsTelemetry()
{
    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_zero;
    telemetry.which_variant = meshtastic_Telemetry_local_stats_tag;
    telemetry.variant.local_stats = meshtastic_LocalStats_init_zero;
    telemetry.time = getTime();
    telemetry.variant.local_stats.uptime_seconds = getUptimeSeconds();
    telemetry.variant.local_stats.channel_utilization = airTime->channelUtilizationPercent();
    telemetry.variant.local_stats.air_util_tx = airTime->utilizationTXPercent();
    telemetry.variant.local_stats.num_online_nodes = numOnlineNodes;
    telemetry.variant.local_stats.num_total_nodes = nodeDB->getNumMeshNodes();
    if (RadioLibInterface::instance) {
        telemetry.variant.local_stats.num_packets_tx = RadioLibInterface::instance->txGood;
        telemetry.variant.local_stats.num_packets_rx = RadioLibInterface::instance->rxGood + RadioLibInterface::instance->rxBad;
        telemetry.variant.local_stats.num_packets_rx_bad = RadioLibInterface::instance->rxBad;
        telemetry.variant.local_stats.num_tx_relay = RadioLibInterface::instance->txRelay;
    }
#ifdef ARCH_PORTDUINO
    if (SimRadio::instance) {
        telemetry.variant.local_stats.num_packets_tx = SimRadio::instance->txGood;
        telemetry.variant.local_stats.num_packets_rx = SimRadio::instance->rxGood + SimRadio::instance->rxBad;
        telemetry.variant.local_stats.num_packets_rx_bad = SimRadio::instance->rxBad;
        telemetry.variant.local_stats.num_tx_relay = SimRadio::instance->txRelay;
    }
#else
    telemetry.variant.local_stats.heap_total_bytes = memGet.getHeapSize();
    telemetry.variant.local_stats.heap_free_bytes = memGet.getFreeHeap();
#endif
    if (router) {
        telemetry.variant.local_stats.num_rx_dupe = router->rxDupe;
        telemetry.variant.local_stats.num_tx_relay_canceled = router->txRelayCanceled;
    }

    LOG_INFO("Sending local stats: uptime=%i, channel_utilization=%f, air_util_tx=%f, num_online_nodes=%i, num_total_nodes=%i",
             telemetry.variant.local_stats.uptime_seconds, telemetry.variant.local_stats.channel_utilization,
             telemetry.variant.local_stats.air_util_tx, telemetry.variant.local_stats.num_online_nodes,
             telemetry.variant.local_stats.num_total_nodes);

    LOG_INFO("num_packets_tx=%i, num_packets_rx=%i, num_packets_rx_bad=%i", telemetry.variant.local_stats.num_packets_tx,
             telemetry.variant.local_stats.num_packets_rx, telemetry.variant.local_stats.num_packets_rx_bad);

    return telemetry;
}

void DeviceTelemetryModule::sendLocalStatsToPhone()
{
    meshtastic_MeshPacket *p = allocDataProtobuf(getLocalStatsTelemetry());
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    service->sendToPhone(p);
}

bool DeviceTelemetryModule::sendTelemetry(NodeNum dest, bool phoneOnly)
{
    // Send normal device telemetry
    meshtastic_Telemetry telemetry = getDeviceTelemetry();
    LOG_INFO("Send: air_util_tx=%f, channel_utilization=%f, battery_level=%i, voltage=%f, uptime=%i",
             telemetry.variant.device_metrics.air_util_tx, telemetry.variant.device_metrics.channel_utilization,
             telemetry.variant.device_metrics.battery_level, telemetry.variant.device_metrics.voltage,
             telemetry.variant.device_metrics.uptime_seconds);

    meshtastic_MeshPacket *p = allocDataProtobuf(telemetry);
    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    nodeDB->updateTelemetry(nodeDB->getNodeNum(), telemetry, RX_SRC_LOCAL);
    if (phoneOnly) {
        LOG_INFO("Send packet to phone");
        service->sendToPhone(p);
    } else {
        LOG_INFO("Send packet to mesh");
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }

    // Also send memory stats as environment metrics for client compatibility
    meshtastic_Telemetry memoryTelemetry = getMemoryStatsAsEnvironmentTelemetry();
    meshtastic_MeshPacket *memoryPacket = allocDataProtobuf(memoryTelemetry);
    memoryPacket->to = dest;
    memoryPacket->decoded.want_response = false;
    memoryPacket->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    nodeDB->updateTelemetry(nodeDB->getNodeNum(), memoryTelemetry, RX_SRC_LOCAL);
    if (phoneOnly) {
        LOG_INFO("Send memory stats packet to phone");
        service->sendToPhone(memoryPacket);
    } else {
        LOG_INFO("Send memory stats packet to mesh");
        service->sendToMesh(memoryPacket, RX_SRC_LOCAL, true);
    }

    return true;
}