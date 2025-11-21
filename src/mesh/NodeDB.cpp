#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_GPS
#include "GPS.h"
#endif
#include "../detect/ScanI2C.h"
#include "Channels.h"
#include "CryptoEngine.h"
#include "Default.h"
#include "FSCommon.h"
#include "MeshRadio.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PacketHistory.h"
#include "PowerFSM.h"
#include "RTC.h"
#include "Router.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "TypeConversions.h"
#include "error.h"
#include "main.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include "modules/NeighborInfoModule.h"
#include <ErriezCRC32.h>
#include <algorithm>
#include <pb_decode.h>
#include <pb_encode.h>
#include <vector>

// Include LittleFS headers for extended filesystem support (if enabled)
#ifdef USE_EXTENDED_FS_FOR_NODEDB
#include "../../platform/stm32wl/littlefs/lfs.h"
#endif

#ifdef ARCH_ESP32
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "SPILock.h"
#include "modules/StoreForwardModule.h"
#include <Preferences.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <nvs_flash.h>
#include <soc/efuse_reg.h>
#include <soc/soc.h>
#endif

#ifdef ARCH_PORTDUINO
#include "modules/StoreForwardModule.h"
#include "platform/portduino/PortduinoGlue.h"
#endif

#ifdef ARCH_NRF52
#include <bluefruit.h>
#include <utility/bonding.h>
#endif

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WIFI
#include <WiFiOTA.h>
#endif

NodeDB *nodeDB = nullptr;

// we have plenty of ram so statically alloc this tempbuf (for now)
EXT_RAM_BSS_ATTR meshtastic_DeviceState devicestate;
meshtastic_MyNodeInfo &myNodeInfo = devicestate.my_node;
meshtastic_NodeDatabase nodeDatabase;
meshtastic_LocalConfig config;
meshtastic_DeviceUIConfig uiconfig{.screen_brightness = 153, .screen_timeout = 30};
meshtastic_LocalModuleConfig moduleConfig;
meshtastic_ChannelFile channelFile;

#ifdef USERPREFS_USE_ADMIN_KEY_0
static unsigned char userprefs_admin_key_0[] = USERPREFS_USE_ADMIN_KEY_0;
#endif
#ifdef USERPREFS_USE_ADMIN_KEY_1
static unsigned char userprefs_admin_key_1[] = USERPREFS_USE_ADMIN_KEY_1;
#endif
#ifdef USERPREFS_USE_ADMIN_KEY_2
static unsigned char userprefs_admin_key_2[] = USERPREFS_USE_ADMIN_KEY_2;
#endif

#ifdef HELTEC_MESH_NODE_T114

uint32_t read8(uint8_t bits, uint8_t dummy, uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    uint32_t ret = 0;
    uint8_t SDAPIN = mosi;
    pinMode(SDAPIN, INPUT_PULLUP);
    digitalWrite(dc, HIGH);
    for (int i = 0; i < dummy; i++) { // any dummy clocks
        digitalWrite(sck, HIGH);
        delay(1);
        digitalWrite(sck, LOW);
        delay(1);
    }
    for (int i = 0; i < bits; i++) { // read results
        ret <<= 1;
        delay(1);
        if (digitalRead(SDAPIN))
            ret |= 1;
        ;
        digitalWrite(sck, HIGH);
        delay(1);
        digitalWrite(sck, LOW);
    }
    return ret;
}

void write9(uint8_t val, uint8_t dc_val, uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    pinMode(mosi, OUTPUT);
    digitalWrite(dc, dc_val);
    for (int i = 0; i < 8; i++) { // send command
        digitalWrite(mosi, (val & 0x80) != 0);
        delay(1);
        digitalWrite(sck, HIGH);
        delay(1);
        digitalWrite(sck, LOW);
        val <<= 1;
    }
}

uint32_t readwrite8(uint8_t cmd, uint8_t bits, uint8_t dummy, uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    digitalWrite(cs, LOW);
    write9(cmd, 0, cs, sck, mosi, dc, rst);
    uint32_t ret = read8(bits, dummy, cs, sck, mosi, dc, rst);
    digitalWrite(cs, HIGH);
    return ret;
}

uint32_t get_st7789_id(uint8_t cs, uint8_t sck, uint8_t mosi, uint8_t dc, uint8_t rst)
{
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    pinMode(cs, OUTPUT);
    pinMode(sck, OUTPUT);
    pinMode(mosi, OUTPUT);
    pinMode(dc, OUTPUT);
    pinMode(rst, OUTPUT);
    digitalWrite(rst, LOW); // Hardware Reset
    delay(10);
    digitalWrite(rst, HIGH);
    delay(10);

    uint32_t ID = 0;
    ID = readwrite8(0x04, 24, 1, cs, sck, mosi, dc, rst);
    ID = readwrite8(0x04, 24, 1, cs, sck, mosi, dc, rst); // ST7789 needs twice
    return ID;
}

#endif

bool meshtastic_NodeDatabase_callback(pb_istream_t *istream, pb_ostream_t *ostream, const pb_field_iter_t *field)
{
    if (ostream) {
        std::vector<meshtastic_NodeInfoLite> const *vec = (std::vector<meshtastic_NodeInfoLite> *)field->pData;
        for (auto item : *vec) {
            if (!pb_encode_tag_for_field(ostream, field))
                return false;
            pb_encode_submessage(ostream, meshtastic_NodeInfoLite_fields, &item);
        }
    }
    if (istream) {
        meshtastic_NodeInfoLite node; // this gets good data
        std::vector<meshtastic_NodeInfoLite> *vec = (std::vector<meshtastic_NodeInfoLite> *)field->pData;

        // CRITICAL FIX: Reserve capacity on first node to avoid multiple reallocations
        // Each push_back() without capacity causes reallocation = double RAM usage temporarily
        if (vec->empty() && vec->capacity() < MAX_NUM_NODES) {
            vec->reserve(MAX_NUM_NODES);
        }

        if (istream->bytes_left && pb_decode(istream, meshtastic_NodeInfoLite_fields, &node))
            vec->push_back(node);
    }
    return true;
}

/** The current change # for radio settings.  Starts at 0 on boot and any time the radio settings
 * might have changed is incremented.  Allows others to detect they might now be on a new channel.
 */
uint32_t radioGeneration;

// FIXME - move this somewhere else
extern void getMacAddr(uint8_t *dmac);

/**
 *
 * Normally userids are unique and start with +country code to look like Signal phone numbers.
 * But there are some special ids used when we haven't yet been configured by a user.  In that case
 * we use !macaddr (no colons).
 */
meshtastic_User &owner = devicestate.owner;
meshtastic_Position localPosition = meshtastic_Position_init_default;
meshtastic_CriticalErrorCode error_code =
    meshtastic_CriticalErrorCode_NONE; // For the error code, only show values from this boot (discard value from flash)
uint32_t error_address = 0;

static uint8_t ourMacAddr[6];

NodeDB::NodeDB()
{
    LOG_INFO("Init NodeDB");
    loadFromDisk();
    cleanupMeshDB();

    uint32_t devicestateCRC = crc32Buffer(&devicestate, sizeof(devicestate));
    uint32_t nodeDatabaseCRC = crc32Buffer(&nodeDatabase, sizeof(nodeDatabase));
    uint32_t configCRC = crc32Buffer(&config, sizeof(config));
    uint32_t channelFileCRC = crc32Buffer(&channelFile, sizeof(channelFile));

    int saveWhat = 0;
    // Get device unique id
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
    uint32_t unique_id[4];
    // ESP32 factory burns a unique id in efuse for S2+ series and evidently C3+ series
    // This is used for HMACs in the esp-rainmaker AIOT platform and seems to be a good choice for us
    esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, unique_id, sizeof(unique_id) * 8);
    if (err == ESP_OK) {
        memcpy(myNodeInfo.device_id.bytes, unique_id, sizeof(unique_id));
        myNodeInfo.device_id.size = 16;
    } else {
        LOG_WARN("Failed to read unique id from efuse");
    }
#elif defined(ARCH_NRF52)
    // Nordic applies a FIPS compliant Random ID to each chip at the factory
    // We concatenate the device address to the Random ID to create a unique ID for now
    // This will likely utilize a crypto module in the future
    uint64_t device_id_start = ((uint64_t)NRF_FICR->DEVICEID[1] << 32) | NRF_FICR->DEVICEID[0];
    uint64_t device_id_end = ((uint64_t)NRF_FICR->DEVICEADDR[1] << 32) | NRF_FICR->DEVICEADDR[0];
    memcpy(myNodeInfo.device_id.bytes, &device_id_start, sizeof(device_id_start));
    memcpy(myNodeInfo.device_id.bytes + sizeof(device_id_start), &device_id_end, sizeof(device_id_end));
    myNodeInfo.device_id.size = 16;
    // Uncomment below to print the device id

#else
    // FIXME - implement for other platforms
#endif

    // if (myNodeInfo.device_id.size == 16) {
    //     std::string deviceIdHex;
    //     for (size_t i = 0; i < myNodeInfo.device_id.size; ++i) {
    //         char buf[3];
    //         snprintf(buf, sizeof(buf), "%02X", myNodeInfo.device_id.bytes[i]);
    //         deviceIdHex += buf;
    //     }
    //     LOG_DEBUG("Device ID (HEX): %s", deviceIdHex.c_str());
    // }

    // likewise - we always want the app requirements to come from the running appload
    myNodeInfo.min_app_version = 30200; // format is Mmmss (where M is 1+the numeric major number. i.e. 30200 means 2.2.00
    // Note! We do this after loading saved settings, so that if somehow an invalid nodenum was stored in preferences we won't
    // keep using that nodenum forever. Crummy guess at our nodenum (but we will check against the nodedb to avoid conflicts)
    pickNewNodeNum();

    // Set our board type so we can share it with others
    owner.hw_model = HW_VENDOR;
    // Ensure user (nodeinfo) role is set to whatever we're configured to
    owner.role = config.device.role;
    // Ensure macaddr is set to our macaddr as it will be copied in our info below
    memcpy(owner.macaddr, ourMacAddr, sizeof(owner.macaddr));

    if (!config.has_security) {
        config.has_security = true;
        config.security = meshtastic_Config_SecurityConfig_init_default;
        config.security.serial_enabled = config.device.serial_enabled;
        config.security.is_managed = config.device.is_managed;
    }

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)

    if (!owner.is_licensed && config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET) {
        bool keygenSuccess = false;
        if (config.security.private_key.size == 32) {
            if (crypto->regeneratePublicKey(config.security.public_key.bytes, config.security.private_key.bytes)) {
                keygenSuccess = true;
            }
        } else {
            LOG_INFO("Generate new PKI keys");
            crypto->generateKeyPair(config.security.public_key.bytes, config.security.private_key.bytes);
            keygenSuccess = true;
        }
        if (keygenSuccess) {
            config.security.public_key.size = 32;
            config.security.private_key.size = 32;
            owner.public_key.size = 32;
            memcpy(owner.public_key.bytes, config.security.public_key.bytes, 32);
            keyIsLowEntropy = checkLowEntropyPublicKey(owner.public_key);
        }
    }
#elif !(MESHTASTIC_EXCLUDE_PKI)
    // Calculate Curve25519 public and private keys
    if (config.security.private_key.size == 32 && config.security.public_key.size == 32) {
        owner.public_key.size = config.security.public_key.size;
        memcpy(owner.public_key.bytes, config.security.public_key.bytes, config.security.public_key.size);
        crypto->setDHPrivateKey(config.security.private_key.bytes);
        keyIsLowEntropy = checkLowEntropyPublicKey(owner.public_key);
    }
#endif
    if (keyIsLowEntropy) {
        LOG_WARN(LOW_ENTROPY_WARNING);
    }
    // Include our owner in the node db under our nodenum
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(getNodeNum());
    info->user = TypeConversions::ConvertToUserLite(owner);
    info->has_user = true;

    // If node database has not been saved for the first time, save it now
#ifdef FSCom
    if (!FSCom.exists(nodeDatabaseFileName)) {
        saveNodeDatabaseToDisk();
    }
#endif

#ifdef ARCH_ESP32
    Preferences preferences;
    preferences.begin("meshtastic", false);
    myNodeInfo.reboot_count = preferences.getUInt("rebootCounter", 0);
    preferences.end();
    LOG_DEBUG("Number of Device Reboots: %d", myNodeInfo.reboot_count);
#endif

    resetRadioConfig(); // If bogus settings got saved, then fix them
    // nodeDB->LOG_DEBUG("region=%d, NODENUM=0x%x, dbsize=%d", config.lora.region, myNodeInfo.my_node_num, numMeshNodes);

    // Uncomment below to always enable UDP broadcasts
    // config.network.enabled_protocols = meshtastic_Config_NetworkConfig_ProtocolFlags_UDP_BROADCAST;

    // If we are setup to broadcast on the default channel, ensure that the telemetry intervals are coerced to the minimum value
    // of 30 minutes or more
    if (channels.isDefaultChannel(channels.getPrimaryIndex())) {
        LOG_DEBUG("Coerce telemetry to min of 30 minutes on defaults");
        moduleConfig.telemetry.device_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.device_update_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.environment_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.environment_update_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.air_quality_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.air_quality_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.power_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.power_update_interval, min_default_telemetry_interval_secs);
        moduleConfig.telemetry.health_update_interval = Default::getConfiguredOrMinimumValue(
            moduleConfig.telemetry.health_update_interval, min_default_telemetry_interval_secs);
    }
    if (moduleConfig.mqtt.has_map_report_settings &&
        moduleConfig.mqtt.map_report_settings.publish_interval_secs < default_map_publish_interval_secs) {
        moduleConfig.mqtt.map_report_settings.publish_interval_secs = default_map_publish_interval_secs;
    }

    // Ensure that the neighbor info update interval is coerced to the minimum
    moduleConfig.neighbor_info.update_interval =
        Default::getConfiguredOrMinimumValue(moduleConfig.neighbor_info.update_interval, min_neighbor_info_broadcast_secs);

    // Don't let licensed users to rebroadcast encrypted packets
    if (owner.is_licensed) {
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
    }

    if (devicestateCRC != crc32Buffer(&devicestate, sizeof(devicestate)))
        saveWhat |= SEGMENT_DEVICESTATE;
    if (nodeDatabaseCRC != crc32Buffer(&nodeDatabase, sizeof(nodeDatabase)))
        saveWhat |= SEGMENT_NODEDATABASE;
    if (configCRC != crc32Buffer(&config, sizeof(config)))
        saveWhat |= SEGMENT_CONFIG;
    if (channelFileCRC != crc32Buffer(&channelFile, sizeof(channelFile)))
        saveWhat |= SEGMENT_CHANNELS;

    if (config.position.gps_enabled) {
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
        config.position.gps_enabled = 0;
    }
#ifdef USERPREFS_FIXED_GPS
    if (myNodeInfo.reboot_count == 1) { // Check if First boot ever or after Factory Reset.
        meshtastic_Position fixedGPS = meshtastic_Position_init_default;
#ifdef USERPREFS_FIXED_GPS_LAT
        fixedGPS.latitude_i = (int32_t)(USERPREFS_FIXED_GPS_LAT * 1e7);
        fixedGPS.has_latitude_i = true;
#endif
#ifdef USERPREFS_FIXED_GPS_LON
        fixedGPS.longitude_i = (int32_t)(USERPREFS_FIXED_GPS_LON * 1e7);
        fixedGPS.has_longitude_i = true;
#endif
#ifdef USERPREFS_FIXED_GPS_ALT
        fixedGPS.altitude = USERPREFS_FIXED_GPS_ALT;
        fixedGPS.has_altitude = true;
#endif
#if defined(USERPREFS_FIXED_GPS_LAT) && defined(USERPREFS_FIXED_GPS_LON)
        fixedGPS.location_source = meshtastic_Position_LocSource_LOC_MANUAL;
        config.has_position = true;
        info->has_position = true;
        info->position = TypeConversions::ConvertToPositionLite(fixedGPS);
        nodeDB->setLocalPosition(fixedGPS);
        config.position.fixed_position = true;
#endif
    }
#endif
    saveToDisk(saveWhat);
}

/**
 * Most (but not always) of the time we want to treat packets 'from' the local phone (where from == 0), as if they originated on
 * the local node. If from is zero this function returns our node number instead
 */
NodeNum getFrom(const meshtastic_MeshPacket *p)
{
    return (p->from == 0) ? nodeDB->getNodeNum() : p->from;
}

// Returns true if the packet originated from the local node
bool isFromUs(const meshtastic_MeshPacket *p)
{
    return p->from == 0 || p->from == nodeDB->getNodeNum();
}

// Returns true if the packet is destined to us
bool isToUs(const meshtastic_MeshPacket *p)
{
    return p->to == nodeDB->getNodeNum();
}

bool isBroadcast(uint32_t dest)
{
    return dest == NODENUM_BROADCAST || dest == NODENUM_BROADCAST_NO_LORA;
}

void NodeDB::resetRadioConfig(bool is_fresh_install)
{
    if (is_fresh_install) {
        radioGeneration++;
    }

    if (channelFile.channels_count != MAX_NUM_CHANNELS) {
        LOG_INFO("Set default channel and radio preferences!");

        channels.initDefaults();
    }

    channels.onConfigChanged();

    // Update the global myRegion
    initRegion();
}

bool NodeDB::factoryReset(bool eraseBleBonds)
{
    LOG_INFO("Perform factory reset!");
    // first, remove the "/prefs" (this removes most prefs)
    spiLock->lock();
    rmDir("/prefs"); // this uses spilock internally...

#ifdef FSCom
    if (FSCom.exists("/static/rangetest.csv") && !FSCom.remove("/static/rangetest.csv")) {
        LOG_ERROR("Could not remove rangetest.csv file");
    }
#endif
    spiLock->unlock();
    // second, install default state (this will deal with the duplicate mac address issue)
    installDefaultNodeDatabase();
    installDefaultDeviceState();
    installDefaultConfig(!eraseBleBonds); // Also preserve the private key if we're not erasing BLE bonds
    installDefaultModuleConfig();
    installDefaultChannels();
    // third, write everything to disk
    saveToDisk();
    if (eraseBleBonds) {
        LOG_INFO("Erase BLE bonds");
#ifdef ARCH_ESP32
        // This will erase what's in NVS including ssl keys, persistent variables and ble pairing
        nvs_flash_erase();
#endif
#ifdef ARCH_NRF52
        LOG_INFO("Clear bluetooth bonds!");
        bond_print_list(BLE_GAP_ROLE_PERIPH);
        bond_print_list(BLE_GAP_ROLE_CENTRAL);
        Bluefruit.Periph.clearBonds();
        Bluefruit.Central.clearBonds();
#endif
    }
    return true;
}

void NodeDB::installDefaultNodeDatabase()
{
    LOG_DEBUG("Install default NodeDatabase");
    nodeDatabase.version = DEVICESTATE_CUR_VER;
    // CRITICAL FIX: Don't pre-allocate 500 nodes (125KB RAM!)
    // Reserve capacity but don't initialize elements
    nodeDatabase.nodes.clear();
    nodeDatabase.nodes.reserve(MAX_NUM_NODES);
    numMeshNodes = 0;
    meshNodes = &nodeDatabase.nodes;
}

void NodeDB::installDefaultConfig(bool preserveKey = false)
{
    uint8_t private_key_temp[32];
    bool shouldPreserveKey = preserveKey && config.has_security && config.security.private_key.size > 0;
    if (shouldPreserveKey) {
        memcpy(private_key_temp, config.security.private_key.bytes, config.security.private_key.size);
    }
    LOG_INFO("Install default LocalConfig");
    memset(&config, 0, sizeof(meshtastic_LocalConfig));
    config.version = DEVICESTATE_CUR_VER;
    config.has_device = true;
    config.has_display = true;
    config.has_lora = true;
    config.has_position = true;
    config.has_power = true;
    config.has_network = true;
    config.has_bluetooth = (HAS_BLUETOOTH ? true : false);
    config.has_security = true;
    config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_ALL;

    config.lora.sx126x_rx_boosted_gain = true;
    config.lora.tx_enabled =
        true; // FIXME: maybe false in the future, and setting region to enable it. (unset region forces it off)
    config.lora.override_duty_cycle = false;
    config.lora.config_ok_to_mqtt = false;

#ifdef USERPREFS_CONFIG_DEVICE_ROLE
    // Restrict ROUTER*, LOST AND FOUND, and REPEATER roles for security reasons
    if (IS_ONE_OF(USERPREFS_CONFIG_DEVICE_ROLE, meshtastic_Config_DeviceConfig_Role_ROUTER,
                  meshtastic_Config_DeviceConfig_Role_ROUTER_LATE, meshtastic_Config_DeviceConfig_Role_REPEATER,
                  meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND)) {
        LOG_WARN("ROUTER roles are restricted, falling back to CLIENT role");
        config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    } else {
        config.device.role = USERPREFS_CONFIG_DEVICE_ROLE;
    }
#else
    config.device.role = meshtastic_Config_DeviceConfig_Role_CLIENT; // Default to client.
#endif

#ifdef USERPREFS_CONFIG_LORA_REGION
    config.lora.region = USERPREFS_CONFIG_LORA_REGION;
#else
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_UNSET;
#endif
#ifdef USERPREFS_LORACONFIG_MODEM_PRESET
    config.lora.modem_preset = USERPREFS_LORACONFIG_MODEM_PRESET;
#else
    config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
#endif
    config.lora.hop_limit = HOP_RELIABLE;
#ifdef USERPREFS_CONFIG_LORA_IGNORE_MQTT
    config.lora.ignore_mqtt = USERPREFS_CONFIG_LORA_IGNORE_MQTT;
#else
    config.lora.ignore_mqtt = false;
#endif
    // Initialize admin_key_count to zero
    byte numAdminKeys = 0;

#ifdef USERPREFS_USE_ADMIN_KEY_0
    // Check if USERPREFS_ADMIN_KEY_0 is non-empty
    if (sizeof(userprefs_admin_key_0) > 0) {
        memcpy(config.security.admin_key[0].bytes, userprefs_admin_key_0, 32);
        config.security.admin_key[0].size = 32;
        numAdminKeys++;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_1
    // Check if USERPREFS_ADMIN_KEY_1 is non-empty
    if (sizeof(userprefs_admin_key_1) > 0) {
        memcpy(config.security.admin_key[1].bytes, userprefs_admin_key_1, 32);
        config.security.admin_key[1].size = 32;
        numAdminKeys++;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_2
    // Check if USERPREFS_ADMIN_KEY_2 is non-empty
    if (sizeof(userprefs_admin_key_2) > 0) {
        memcpy(config.security.admin_key[2].bytes, userprefs_admin_key_2, 32);
        config.security.admin_key[2].size = 32;
        numAdminKeys++;
    }
#endif
    config.security.admin_key_count = numAdminKeys;

    if (shouldPreserveKey) {
        config.security.private_key.size = 32;
        memcpy(config.security.private_key.bytes, private_key_temp, config.security.private_key.size);
        printBytes("Restored key", config.security.private_key.bytes, config.security.private_key.size);
    } else {
        config.security.private_key.size = 0;
    }
    config.security.public_key.size = 0;
    
    // CRITICAL FIX: Generate private key if it's empty (even if region is UNSET)
    // This ensures the node always has a private key, which is required for node identity
    // The key will be used when region is set later, or can be used for other purposes
#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    if (config.security.private_key.size == 0 && crypto != nullptr) {
        LOG_INFO("Generating new private key in installDefaultConfig() (region may be UNSET)");
        crypto->generateKeyPair(config.security.public_key.bytes, config.security.private_key.bytes);
        config.security.public_key.size = 32;
        config.security.private_key.size = 32;
        LOG_DEBUG("Private key generated successfully (size: %d)", config.security.private_key.size);
    }
#endif
#ifdef PIN_GPS_EN
    config.position.gps_en_gpio = PIN_GPS_EN;
#endif
#ifdef GPS_POWER_TOGGLE
    config.device.disable_triple_click = false;
#else
    config.device.disable_triple_click = true;
#endif
#if defined(USERPREFS_CONFIG_GPS_MODE)
    config.position.gps_mode = USERPREFS_CONFIG_GPS_MODE;
#elif !HAS_GPS || GPS_DEFAULT_NOT_PRESENT
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
#elif !defined(GPS_RX_PIN)
    if (config.position.rx_gpio == 0)
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
    else
        config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_DISABLED;
#else
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;
#endif
#ifdef USERPREFS_CONFIG_SMART_POSITION_ENABLED
    config.position.position_broadcast_smart_enabled = USERPREFS_CONFIG_SMART_POSITION_ENABLED;
#else
    config.position.position_broadcast_smart_enabled = true;
#endif
    config.position.broadcast_smart_minimum_distance = 100;
    config.position.broadcast_smart_minimum_interval_secs = 30;
    if (config.device.role != meshtastic_Config_DeviceConfig_Role_ROUTER)
        config.device.node_info_broadcast_secs = default_node_info_broadcast_secs;
    config.security.serial_enabled = true;
    config.security.admin_channel_enabled = false;
    resetRadioConfig(true); // This also triggers NodeInfo/Position requests since we're fresh
    strncpy(config.network.ntp_server, "meshtastic.pool.ntp.org", 32);

#if (defined(T_DECK) || defined(T_WATCH_S3) || defined(UNPHONE) || defined(PICOMPUTER_S3) || defined(SENSECAP_INDICATOR) ||      \
     defined(ELECROW_PANEL)) &&                                                                                                  \
    HAS_TFT
    // switch BT off by default; use TFT programming mode or hotkey to enable
    config.bluetooth.enabled = false;
#else
    // default to bluetooth capability of platform as default
    config.bluetooth.enabled = true;
#endif
    config.bluetooth.fixed_pin = defaultBLEPin;

#if defined(ST7735_CS) || defined(USE_EINK) || defined(ILI9341_DRIVER) || defined(ILI9342_DRIVER) || defined(ST7789_CS) ||       \
    defined(HX8357_CS) || defined(USE_ST7789) || defined(ILI9488_CS)
    bool hasScreen = true;
#ifdef HELTEC_MESH_NODE_T114
    uint32_t st7789_id = get_st7789_id(ST7789_NSS, ST7789_SCK, ST7789_SDA, ST7789_RS, ST7789_RESET);
    if (st7789_id == 0xFFFFFF) {
        hasScreen = false;
    }
#endif
#elif ARCH_PORTDUINO
    bool hasScreen = false;
    if (settingsMap[displayPanel])
        hasScreen = true;
    else
        hasScreen = screen_found.port != ScanI2C::I2CPort::NO_I2C;
#elif MESHTASTIC_INCLUDE_NICHE_GRAPHICS // See "src/graphics/niche"
    bool hasScreen = true; // Use random pin for Bluetooth pairing
#else
    bool hasScreen = screen_found.port != ScanI2C::I2CPort::NO_I2C;
#endif

#ifdef USERPREFS_FIXED_BLUETOOTH
    config.bluetooth.fixed_pin = USERPREFS_FIXED_BLUETOOTH;
    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
#else
    config.bluetooth.mode = hasScreen ? meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN
                                      : meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
#endif
    // for backward compat, default position flags are ALT+MSL
    config.position.position_flags =
        (meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE_MSL |
         meshtastic_Config_PositionConfig_PositionFlags_SPEED | meshtastic_Config_PositionConfig_PositionFlags_HEADING |
         meshtastic_Config_PositionConfig_PositionFlags_DOP | meshtastic_Config_PositionConfig_PositionFlags_SATINVIEW);

// Set default value for 'Mesh via UDP'
#if HAS_UDP_MULTICAST
#ifdef USERPREFS_NETWORK_ENABLED_PROTOCOLS
    config.network.enabled_protocols = USERPREFS_NETWORK_ENABLED_PROTOCOLS;
#else
    config.network.enabled_protocols = 1;
#endif
#endif

#ifdef USERPREFS_NETWORK_WIFI_ENABLED
    config.network.wifi_enabled = USERPREFS_NETWORK_WIFI_ENABLED;
#endif

#ifdef USERPREFS_NETWORK_WIFI_SSID
    strncpy(config.network.wifi_ssid, USERPREFS_NETWORK_WIFI_SSID, sizeof(config.network.wifi_ssid));
#endif

#ifdef USERPREFS_NETWORK_WIFI_PSK
    strncpy(config.network.wifi_psk, USERPREFS_NETWORK_WIFI_PSK, sizeof(config.network.wifi_psk));
#endif

#ifdef DISPLAY_FLIP_SCREEN
    config.display.flip_screen = true;
#endif
#ifdef RAK4630
    config.display.wake_on_tap_or_motion = true;
#endif
#if defined(T_WATCH_S3) || defined(SENSECAP_INDICATOR)
    config.display.screen_on_secs = 30;
    config.display.wake_on_tap_or_motion = true;
#endif

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_WIFI
    if (WiFiOTA::isUpdated()) {
        WiFiOTA::recoverConfig(&config.network);
    }
#endif

#ifdef USERPREFS_CONFIG_DEVICE_ROLE
    // Apply role-specific defaults when role is set via user preferences
    installRoleDefaults(config.device.role);
#endif

    initConfigIntervals();
}

void NodeDB::initConfigIntervals()
{
#ifdef USERPREFS_CONFIG_GPS_UPDATE_INTERVAL
    config.position.gps_update_interval = USERPREFS_CONFIG_GPS_UPDATE_INTERVAL;
#else
    config.position.gps_update_interval = default_gps_update_interval;
#endif
#ifdef USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL
    config.position.position_broadcast_secs = USERPREFS_CONFIG_POSITION_BROADCAST_INTERVAL;
#else
    config.position.position_broadcast_secs = default_broadcast_interval_secs;
#endif

    config.power.ls_secs = default_ls_secs;
    config.power.min_wake_secs = default_min_wake_secs;
    config.power.sds_secs = default_sds_secs;
    config.power.wait_bluetooth_secs = default_wait_bluetooth_secs;

    config.display.screen_on_secs = default_screen_on_secs;

#if defined(USE_POWERSAVE)
    config.power.is_power_saving = true;
    config.display.screen_on_secs = 30;
    config.power.wait_bluetooth_secs = 30;
#endif
}

void NodeDB::installDefaultModuleConfig()
{
    LOG_INFO("Install default ModuleConfig");
    memset(&moduleConfig, 0, sizeof(meshtastic_ModuleConfig));

    moduleConfig.version = DEVICESTATE_CUR_VER;
    moduleConfig.has_mqtt = true;
    moduleConfig.has_range_test = true;
    moduleConfig.has_serial = true;
    moduleConfig.has_store_forward = true;
    moduleConfig.has_telemetry = true;
    moduleConfig.has_external_notification = true;
#if defined(PIN_BUZZER)
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.output_buzzer = PIN_BUZZER;
    moduleConfig.external_notification.use_pwm = true;
    moduleConfig.external_notification.alert_message_buzzer = true;
    moduleConfig.external_notification.nag_timeout = 60;
#endif
#if defined(RAK4630) || defined(RAK11310)
    // Default to RAK led pin 2 (blue)
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.output = PIN_LED2;
    moduleConfig.external_notification.active = true;
    moduleConfig.external_notification.alert_message = true;
    moduleConfig.external_notification.output_ms = 1000;
    moduleConfig.external_notification.nag_timeout = 60;
#endif

#ifdef HAS_I2S
    // Don't worry about the other settings for T-Watch, we'll also use the DRV2056 behavior for notifications
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.use_i2s_as_buzzer = true;
    moduleConfig.external_notification.alert_message_buzzer = true;
#if HAS_TFT
    if (moduleConfig.external_notification.nag_timeout == 60)
        moduleConfig.external_notification.nag_timeout = 0;
#else
    moduleConfig.external_notification.nag_timeout = 60;
#endif
#endif
#ifdef NANO_G2_ULTRA
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.alert_message = true;
    moduleConfig.external_notification.output_ms = 100;
    moduleConfig.external_notification.active = true;
#endif
#ifdef ELECROW_ThinkNode_M1
    // Default to Elecrow USER_LED (blue)
    moduleConfig.external_notification.enabled = true;
    moduleConfig.external_notification.output = USER_LED;
    moduleConfig.external_notification.active = true;
    moduleConfig.external_notification.alert_message = true;
    moduleConfig.external_notification.output_ms = 1000;
    moduleConfig.external_notification.nag_timeout = 60;
#endif
#ifdef BUTTON_SECONDARY_CANNEDMESSAGES
    // Use a board's second built-in button as input source for canned messages
    moduleConfig.canned_message.enabled = true;
    moduleConfig.canned_message.inputbroker_pin_press = BUTTON_PIN_SECONDARY;
    strcpy(moduleConfig.canned_message.allow_input_source, "scanAndSelect");
#endif

    moduleConfig.has_canned_message = true;

#if USERPREFS_MQTT_ENABLED && !MESHTASTIC_EXCLUDE_MQTT
    moduleConfig.mqtt.enabled = true;
#endif
#ifdef USERPREFS_MQTT_ADDRESS
    strncpy(moduleConfig.mqtt.address, USERPREFS_MQTT_ADDRESS, sizeof(moduleConfig.mqtt.address));
#else
    strncpy(moduleConfig.mqtt.address, default_mqtt_address, sizeof(moduleConfig.mqtt.address));
#endif
#ifdef USERPREFS_MQTT_USERNAME
    strncpy(moduleConfig.mqtt.username, USERPREFS_MQTT_USERNAME, sizeof(moduleConfig.mqtt.username));
#else
    strncpy(moduleConfig.mqtt.username, default_mqtt_username, sizeof(moduleConfig.mqtt.username));
#endif
#ifdef USERPREFS_MQTT_PASSWORD
    strncpy(moduleConfig.mqtt.password, USERPREFS_MQTT_PASSWORD, sizeof(moduleConfig.mqtt.password));
#else
    strncpy(moduleConfig.mqtt.password, default_mqtt_password, sizeof(moduleConfig.mqtt.password));
#endif
#ifdef USERPREFS_MQTT_ROOT_TOPIC
    strncpy(moduleConfig.mqtt.root, USERPREFS_MQTT_ROOT_TOPIC, sizeof(moduleConfig.mqtt.root));
#else
    strncpy(moduleConfig.mqtt.root, default_mqtt_root, sizeof(moduleConfig.mqtt.root));
#endif
#ifdef USERPREFS_MQTT_ENCRYPTION_ENABLED
    moduleConfig.mqtt.encryption_enabled = USERPREFS_MQTT_ENCRYPTION_ENABLED;
#else
    moduleConfig.mqtt.encryption_enabled = default_mqtt_encryption_enabled;
#endif
#ifdef USERPREFS_MQTT_TLS_ENABLED
    moduleConfig.mqtt.tls_enabled = USERPREFS_MQTT_TLS_ENABLED;
#else
    moduleConfig.mqtt.tls_enabled = default_mqtt_tls_enabled;
#endif

    moduleConfig.has_neighbor_info = true;
    moduleConfig.neighbor_info.enabled = false;

    moduleConfig.has_detection_sensor = true;
    moduleConfig.detection_sensor.enabled = false;
    moduleConfig.detection_sensor.detection_trigger_type = meshtastic_ModuleConfig_DetectionSensorConfig_TriggerType_LOGIC_HIGH;
    moduleConfig.detection_sensor.minimum_broadcast_secs = 45;

    moduleConfig.has_ambient_lighting = true;
    moduleConfig.ambient_lighting.current = 10;
    // Default to a color based on our node number
    moduleConfig.ambient_lighting.red = (myNodeInfo.my_node_num & 0xFF0000) >> 16;
    moduleConfig.ambient_lighting.green = (myNodeInfo.my_node_num & 0x00FF00) >> 8;
    moduleConfig.ambient_lighting.blue = myNodeInfo.my_node_num & 0x0000FF;

    initModuleConfigIntervals();
}

void NodeDB::installRoleDefaults(meshtastic_Config_DeviceConfig_Role role)
{
    if (role == meshtastic_Config_DeviceConfig_Role_ROUTER) {
        initConfigIntervals();
        initModuleConfigIntervals();
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY;
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
    } else if (role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
    } else if (role == meshtastic_Config_DeviceConfig_Role_REPEATER) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
        config.display.screen_on_secs = 1;
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_CORE_PORTNUMS_ONLY;
    } else if (role == meshtastic_Config_DeviceConfig_Role_SENSOR) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
        moduleConfig.telemetry.environment_measurement_enabled = true;
        moduleConfig.telemetry.environment_update_interval = 300;
    } else if (role == meshtastic_Config_DeviceConfig_Role_LOST_AND_FOUND) {
        config.position.position_broadcast_smart_enabled = false;
        config.position.position_broadcast_secs = 300; // Every 5 minutes
    } else if (role == meshtastic_Config_DeviceConfig_Role_TAK) {
        config.device.node_info_broadcast_secs = ONE_DAY;
        config.position.position_broadcast_smart_enabled = false;
        config.position.position_broadcast_secs = ONE_DAY;
        // Remove Altitude MSL from flags since CoTs use HAE (height above ellipsoid)
        config.position.position_flags =
            (meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_SPEED |
             meshtastic_Config_PositionConfig_PositionFlags_HEADING | meshtastic_Config_PositionConfig_PositionFlags_DOP);
        moduleConfig.telemetry.device_update_interval = ONE_DAY;
    } else if (role == meshtastic_Config_DeviceConfig_Role_TRACKER) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
    } else if (role == meshtastic_Config_DeviceConfig_Role_TAK_TRACKER) {
        owner.has_is_unmessagable = true;
        owner.is_unmessagable = true;
        config.device.node_info_broadcast_secs = ONE_DAY;
        config.position.position_broadcast_smart_enabled = true;
        config.position.position_broadcast_secs = 3 * 60; // Every 3 minutes
        config.position.broadcast_smart_minimum_distance = 20;
        config.position.broadcast_smart_minimum_interval_secs = 15;
        // Remove Altitude MSL from flags since CoTs use HAE (height above ellipsoid)
        config.position.position_flags =
            (meshtastic_Config_PositionConfig_PositionFlags_ALTITUDE | meshtastic_Config_PositionConfig_PositionFlags_SPEED |
             meshtastic_Config_PositionConfig_PositionFlags_HEADING | meshtastic_Config_PositionConfig_PositionFlags_DOP);
        moduleConfig.telemetry.device_update_interval = ONE_DAY;
    } else if (role == meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN) {
        config.device.rebroadcast_mode = meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY;
        config.device.node_info_broadcast_secs = UINT32_MAX;
        config.position.position_broadcast_smart_enabled = false;
        config.position.position_broadcast_secs = UINT32_MAX;
        moduleConfig.neighbor_info.update_interval = UINT32_MAX;
        moduleConfig.telemetry.device_update_interval = UINT32_MAX;
        moduleConfig.telemetry.environment_update_interval = UINT32_MAX;
        moduleConfig.telemetry.air_quality_interval = UINT32_MAX;
        moduleConfig.telemetry.health_update_interval = UINT32_MAX;
    }
}

void NodeDB::initModuleConfigIntervals()
{
    // Zero out telemetry intervals so that they coalesce to defaults in Default.h
    moduleConfig.telemetry.device_update_interval = 0;
    moduleConfig.telemetry.environment_update_interval = 0;
    moduleConfig.telemetry.air_quality_interval = 0;
    moduleConfig.telemetry.power_update_interval = 0;
    moduleConfig.telemetry.health_update_interval = 0;
    moduleConfig.neighbor_info.update_interval = 0;
    moduleConfig.paxcounter.paxcounter_update_interval = 0;
}

void NodeDB::installDefaultChannels()
{
    LOG_INFO("Install default ChannelFile");
    memset(&channelFile, 0, sizeof(meshtastic_ChannelFile));
    channelFile.version = DEVICESTATE_CUR_VER;
}

void NodeDB::resetNodes()
{
    if (!config.position.fixed_position)
        clearLocalPosition();
    numMeshNodes = 1;
    std::fill(nodeDatabase.nodes.begin() + 1, nodeDatabase.nodes.end(), meshtastic_NodeInfoLite());
    devicestate.has_rx_text_message = false;
    devicestate.has_rx_waypoint = false;
    saveNodeDatabaseToDisk();
    saveDeviceStateToDisk();
    if (neighborInfoModule && moduleConfig.neighbor_info.enabled)
        neighborInfoModule->resetNeighbors();
}

void NodeDB::removeNodeByNum(NodeNum nodeNum)
{
    int newPos = 0, removed = 0;
    for (int i = 0; i < numMeshNodes; i++) {
        if (meshNodes->at(i).num != nodeNum)
            meshNodes->at(newPos++) = meshNodes->at(i);
        else
            removed++;
    }
    numMeshNodes -= removed;
    std::fill(nodeDatabase.nodes.begin() + numMeshNodes, nodeDatabase.nodes.begin() + numMeshNodes + 1,
              meshtastic_NodeInfoLite());
    LOG_DEBUG("NodeDB::removeNodeByNum purged %d entries. Save changes", removed);
    saveNodeDatabaseToDisk();
}

void NodeDB::clearLocalPosition()
{
    meshtastic_NodeInfoLite *node = getMeshNode(nodeDB->getNodeNum());
    node->position.latitude_i = 0;
    node->position.longitude_i = 0;
    node->position.altitude = 0;
    node->position.time = 0;
    setLocalPosition(meshtastic_Position_init_default);
}

void NodeDB::cleanupMeshDB()
{
    int newPos = 0, removed = 0;
    for (int i = 0; i < numMeshNodes; i++) {
        if (meshNodes->at(i).has_user) {
            if (meshNodes->at(i).user.public_key.size > 0) {
                if (memfll(meshNodes->at(i).user.public_key.bytes, 0, meshNodes->at(i).user.public_key.size)) {
                    meshNodes->at(i).user.public_key.size = 0;
                }
            }
            meshNodes->at(newPos++) = meshNodes->at(i);
        } else {
            removed++;
        }
    }
    numMeshNodes -= removed;
    std::fill(nodeDatabase.nodes.begin() + numMeshNodes, nodeDatabase.nodes.begin() + numMeshNodes + removed,
              meshtastic_NodeInfoLite());
    LOG_DEBUG("cleanupMeshDB purged %d entries", removed);
}

void NodeDB::installDefaultDeviceState()
{
    LOG_INFO("Install default DeviceState");
    // memset(&devicestate, 0, sizeof(meshtastic_DeviceState));

    // init our devicestate with valid flags so protobuf writing/reading will work
    devicestate.has_my_node = true;
    devicestate.has_owner = true;
    devicestate.version = DEVICESTATE_CUR_VER;
    devicestate.receive_queue_count = 0; // Not yet implemented FIXME
    devicestate.has_rx_waypoint = false;
    devicestate.has_rx_text_message = false;

    generatePacketId(); // FIXME - ugly way to init current_packet_id;

    // Set default owner name
    pickNewNodeNum(); // based on macaddr now
#ifdef USERPREFS_CONFIG_OWNER_LONG_NAME
    snprintf(owner.long_name, sizeof(owner.long_name), (const char *)USERPREFS_CONFIG_OWNER_LONG_NAME);
#else
    snprintf(owner.long_name, sizeof(owner.long_name), "Meshtastic %04x", getNodeNum() & 0x0ffff);
#endif
#ifdef USERPREFS_CONFIG_OWNER_SHORT_NAME
    snprintf(owner.short_name, sizeof(owner.short_name), (const char *)USERPREFS_CONFIG_OWNER_SHORT_NAME);
#else
    snprintf(owner.short_name, sizeof(owner.short_name), "%04x", getNodeNum() & 0x0ffff);
#endif
    snprintf(owner.id, sizeof(owner.id), "!%08x", getNodeNum()); // Default node ID now based on nodenum
    memcpy(owner.macaddr, ourMacAddr, sizeof(owner.macaddr));
    owner.has_is_unmessagable = true;
    owner.is_unmessagable = false;
}

// We reserve a few nodenums for future use
#define NUM_RESERVED 4

/**
 * get our starting (provisional) nodenum from flash.
 */
void NodeDB::pickNewNodeNum()
{
    NodeNum nodeNum = myNodeInfo.my_node_num;
    getMacAddr(ourMacAddr); // Make sure ourMacAddr is set
    if (nodeNum == 0) {
        // Pick an initial nodenum based on the macaddr
        nodeNum = (ourMacAddr[2] << 24) | (ourMacAddr[3] << 16) | (ourMacAddr[4] << 8) | ourMacAddr[5];
    }

    meshtastic_NodeInfoLite *found;
    while (((found = getMeshNode(nodeNum)) && memcmp(found->user.macaddr, ourMacAddr, sizeof(ourMacAddr)) != 0) ||
           (nodeNum == NODENUM_BROADCAST || nodeNum < NUM_RESERVED)) {
        NodeNum candidate = random(NUM_RESERVED, LONG_MAX); // try a new random choice
        if (found)
            LOG_WARN("NOTE! Our desired nodenum 0x%x is invalid or in use, by MAC ending in 0x%02x%02x vs our 0x%02x%02x, so "
                     "trying for 0x%x",
                     nodeNum, found->user.macaddr[4], found->user.macaddr[5], ourMacAddr[4], ourMacAddr[5], candidate);
        nodeNum = candidate;
    }
    LOG_DEBUG("Use nodenum 0x%x ", nodeNum);

    myNodeInfo.my_node_num = nodeNum;
}

/** Load a protobuf from a file, return LoadFileResult */
LoadFileResult NodeDB::loadProto(const char *filename, size_t protoSize, size_t objSize, const pb_msgdesc_t *fields,
                                 void *dest_struct)
{
    LoadFileResult state = LoadFileResult::OTHER_FAILURE;
#ifdef FSCom
    concurrency::LockGuard g(spiLock);

    // Check if this is nodes.proto and if extended filesystem should be used
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    extern bool useExtendedFSForNodeDB();
    extern bool isNodeDBFile(const char* filename);
    extern void* getExtendedFSForNodeDB();
    
    if (useExtendedFSForNodeDB() && isNodeDBFile(filename)) {
        // Use extended filesystem for nodes.proto
        LOG_INFO("Load %s from EXTENDED filesystem (80 pages, 320 KB)", filename);
        
        // Cast to lfs_t* (headers already included at top of file)
        lfs_t* extended_lfs = (lfs_t*)getExtendedFSForNodeDB();
        if (extended_lfs) {
            // Forward declaration of wrapper class
            class LittleFSFileWrapper {
            private:
                lfs_t* lfs;
                lfs_file_t file;
                bool isOpen;
                bool isWriteMode;
            public:
                LittleFSFileWrapper(lfs_t* fs) : lfs(fs), isOpen(false), isWriteMode(false) {
                    memset(&file, 0, sizeof(lfs_file_t));
                }
                ~LittleFSFileWrapper() { if (isOpen) close(); }
                bool open(const char* path, uint8_t mode) {
                    if (isOpen) close();
                    int flags = 0;
                    if (mode == FILE_O_READ || mode == Adafruit_LittleFS_Namespace::FILE_O_READ) {
                        flags = LFS_O_RDONLY;
                        isWriteMode = false;
                    } else {
                        return false;
                    }
                    int result = lfs_file_open(lfs, &file, path, flags);
                    if (result == LFS_ERR_OK) {
                        isOpen = true;
                        return true;
                    }
                    return false;
                }
                void close() {
                    if (isOpen) {
                        lfs_file_close(lfs, &file);
                        isOpen = false;
                    }
                }
                int read(uint8_t* buf, size_t size) {
                    if (!isOpen || isWriteMode) return 0;
                    lfs_ssize_t result = lfs_file_read(lfs, &file, buf, size);
                    return (result >= 0) ? result : 0;
                }
                int read() {
                    if (!isOpen || isWriteMode) return -1;
                    uint8_t byte;
                    lfs_ssize_t result = lfs_file_read(lfs, &file, &byte, 1);
                    return (result == 1) ? byte : -1;
                }
                bool available() {
                    if (!isOpen || isWriteMode) return false;
                    lfs_soff_t pos = lfs_file_tell(lfs, &file);
                    lfs_soff_t size = lfs_file_size(lfs, &file);
                    return (pos < size);
                }
                operator bool() const { return isOpen; }
            };
            
            // Read file into buffer first, then decode (more reliable than using readcb wrapper)
            // This avoids compatibility issues with File* vs LittleFSFileWrapper*
            LOG_DEBUG("Load: Step 1: Opening file '%s' for reading from extended filesystem...", filename);
            uint32_t load_start_time = millis();
            lfs_file_t file;
            uint32_t open_start = millis();
            int open_result = lfs_file_open(extended_lfs, &file, filename, LFS_O_RDONLY);
            uint32_t open_time = millis() - open_start;
            
            if (open_result == LFS_ERR_OK) {
                LOG_DEBUG("Load: Step 1 SUCCESS: File '%s' opened (took %u ms)", filename, open_time);
                
                // Get file size
                LOG_DEBUG("Load: Step 2: Getting file size for '%s'...", filename);
                uint32_t size_start = millis();
                lfs_soff_t file_size = lfs_file_size(extended_lfs, &file);
                uint32_t size_time = millis() - size_start;
                
                if (file_size >= 0 && file_size <= 512 * 1024) {  // Max 512 KB (safety limit)
                    LOG_DEBUG("Load: Step 2 SUCCESS: File '%s' size: %d bytes (took %u ms)", filename, (int)file_size, size_time);
                    
                    // Allocate buffer for file content
                    LOG_DEBUG("Load: Step 3: Allocating buffer for '%s' (%d bytes)...", filename, (int)file_size);
                    uint32_t alloc_start = millis();
                    uint8_t* file_buffer = (uint8_t*)malloc(file_size);
                    uint32_t alloc_time = millis() - alloc_start;
                    
                    if (file_buffer) {
                        LOG_DEBUG("Load: Step 3 SUCCESS: Buffer allocated (took %u ms)", alloc_time);
                        
                        // Read entire file
                        LOG_DEBUG("Load: Step 4: Reading %d bytes from '%s'...", (int)file_size, filename);
                        uint32_t read_start = millis();
                        lfs_ssize_t read_result = lfs_file_read(extended_lfs, &file, file_buffer, file_size);
                        uint32_t read_time = millis() - read_start;
                        lfs_file_close(extended_lfs, &file);
                        
                        if (read_result == file_size) {
                            LOG_DEBUG("Load: Step 4 SUCCESS: Read %d bytes (took %u ms)", (int)read_result, read_time);
                            
                            // Decode from buffer
                            LOG_DEBUG("Load: Step 5: Decoding protobuf from '%s'...", filename);
                            uint32_t decode_start = millis();
                            pb_istream_t stream = pb_istream_from_buffer(file_buffer, file_size);
                            memset(dest_struct, 0, objSize);
                            if (!pb_decode(&stream, fields, dest_struct)) {
                                LOG_ERROR("Load: Step 5 FAILED: Can't decode protobuf %s: %s", filename, PB_GET_ERROR(&stream));
                                state = LoadFileResult::DECODE_FAILED;
                            } else {
                                uint32_t decode_time = millis() - decode_start;
                                uint32_t total_time = millis() - load_start_time;
                                LOG_INFO("Load: Step 5 SUCCESS: Loaded %s successfully from EXTENDED filesystem (%d bytes, decode: %u ms, total: %u ms)", 
                                        filename, (int)file_size, decode_time, total_time);
                                state = LoadFileResult::LOAD_SUCCESS;
                            }
                        } else {
                            LOG_ERROR("Load: Step 4 FAILED: Read %d of %d bytes from '%s' (took %u ms)", 
                                     (int)read_result, (int)file_size, filename, read_time);
                            state = LoadFileResult::OTHER_FAILURE;
                        }
                        free(file_buffer);
                    } else {
                        LOG_ERROR("Load: Step 3 FAILED: Failed to allocate buffer for '%s' (size: %d bytes, took %u ms)", 
                                 filename, (int)file_size, alloc_time);
                        lfs_file_close(extended_lfs, &file);
                        state = LoadFileResult::OTHER_FAILURE;
                    }
                } else {
                    LOG_ERROR("Load: Step 2 FAILED: File '%s' size invalid or too large: %d bytes (took %u ms)", 
                             filename, (int)file_size, size_time);
                    lfs_file_close(extended_lfs, &file);
                    state = LoadFileResult::OTHER_FAILURE;
                }
            } else {
                // File doesn't exist - normal for first boot, will be created on first save
                LOG_DEBUG("Load: Step 1: File '%s' not found in extended filesystem (error: %d, took %u ms, will be created on first save)", 
                         filename, open_result, open_time);
                state = LoadFileResult::OTHER_FAILURE;  // Return failure so standard code can handle first boot
            }
        } else {
            LOG_ERROR("Extended filesystem not available for %s", filename);
        }
    } else {
    #endif // USE_EXTENDED_FS_FOR_NODEDB
        // Use main filesystem (standard behavior)
        auto f = FSCom.open(filename, FILE_O_READ);

        if (f) {
            LOG_INFO("Load %s", filename);
            pb_istream_t stream = {&readcb, &f, protoSize};

            memset(dest_struct, 0, objSize);
            if (!pb_decode(&stream, fields, dest_struct)) {
                LOG_ERROR("Error: can't decode protobuf %s", PB_GET_ERROR(&stream));
                state = LoadFileResult::DECODE_FAILED;
            } else {
                LOG_INFO("Loaded %s successfully", filename);
                state = LoadFileResult::LOAD_SUCCESS;
            }
            f.close();
        } else {
            LOG_ERROR("Could not open / read %s", filename);
        }
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    }
    #endif // USE_EXTENDED_FS_FOR_NODEDB
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
    state = LoadFileResult::NO_FILESYSTEM;
#endif
    return state;
}

void NodeDB::loadFromDisk()
{
    // Mark the current device state as completely unusable, so that if we fail reading the entire file from
    // disk we will still factoryReset to restore things.
    devicestate.version = 0;

    meshtastic_Config_SecurityConfig backupSecurity = meshtastic_Config_SecurityConfig_init_zero;

#ifdef ARCH_ESP32
    spiLock->lock();
    // If the legacy deviceState exists, start over with a factory reset
    if (FSCom.exists("/static/static"))
        rmDir("/static/static"); // Remove bad static web files bundle from initial 2.5.13 release
    spiLock->unlock();
#endif
#ifdef FSCom
    spiLock->lock();
    if (FSCom.exists(legacyPrefFileName)) {
        spiLock->unlock();
        LOG_WARN("Legacy prefs version found, factory resetting");
        if (loadProto(configFileName, meshtastic_LocalConfig_size, sizeof(meshtastic_LocalConfig), &meshtastic_LocalConfig_msg,
                      &config) == LoadFileResult::LOAD_SUCCESS &&
            config.has_security && config.security.private_key.size > 0) {
            LOG_DEBUG("Saving backup of security config and keys");
            backupSecurity = config.security;
        }
        spiLock->lock();
        rmDir("/prefs");
        spiLock->unlock();
    } else {
        spiLock->unlock();
    }

#endif
    auto state = loadProto(nodeDatabaseFileName, getMaxNodesAllocatedSize(), sizeof(meshtastic_NodeDatabase),
                           &meshtastic_NodeDatabase_msg, &nodeDatabase);
    if (nodeDatabase.version < DEVICESTATE_MIN_VER) {
        LOG_WARN("NodeDatabase %d is old, discard", nodeDatabase.version);
        installDefaultNodeDatabase();
    } else {
        meshNodes = &nodeDatabase.nodes;
        numMeshNodes = nodeDatabase.nodes.size();
        LOG_INFO("Loaded saved nodedatabase version %d, with nodes count: %d", nodeDatabase.version, nodeDatabase.nodes.size());
    }

    if (numMeshNodes > MAX_NUM_NODES) {
        LOG_WARN("Node count %d exceeds MAX_NUM_NODES %d, truncating", numMeshNodes, MAX_NUM_NODES);
        numMeshNodes = MAX_NUM_NODES;
    }

    // static DeviceState scratch; We no longer read into a tempbuf because this structure is 15KB of valuable RAM
    state = loadProto(deviceStateFileName, meshtastic_DeviceState_size, sizeof(meshtastic_DeviceState),
                      &meshtastic_DeviceState_msg, &devicestate);

    // See https://github.com/meshtastic/firmware/issues/4184#issuecomment-2269390786
    // It is very important to try and use the saved prefs even if we fail to read meshtastic_DeviceState.  Because most of our
    // critical config may still be valid (in the other files - loaded next).
    // Also, if we did fail on reading we probably failed on the enormous (and non critical) nodeDB.  So DO NOT install default
    // device state.
    // if (state != LoadFileResult::LOAD_SUCCESS) {
    //    installDefaultDeviceState(); // Our in RAM copy might now be corrupt
    //} else {
    if ((state != LoadFileResult::LOAD_SUCCESS) || (devicestate.version < DEVICESTATE_MIN_VER)) {
        LOG_WARN("Devicestate %d is old or invalid, discard", devicestate.version);
        installDefaultDeviceState();
    } else {
        LOG_INFO("Loaded saved devicestate version %d", devicestate.version);
    }

    state = loadProto(configFileName, meshtastic_LocalConfig_size, sizeof(meshtastic_LocalConfig), &meshtastic_LocalConfig_msg,
                      &config);
    if (state != LoadFileResult::LOAD_SUCCESS) {
        LOG_WARN("config.proto not found - installing default config (this is normal on first boot)");
        installDefaultConfig(); // Our in RAM copy might now be corrupt
        // CRITICAL: Save default config immediately so private key is preserved
        // This ensures config.proto is created on first boot with a valid private key
        LOG_INFO("Saving default config.proto to disk (with new private key)...");
        if (saveToDisk(SEGMENT_CONFIG)) {
            LOG_INFO("Default config.proto saved successfully - private key will persist across reboots");
        } else {
            LOG_ERROR("CRITICAL: Failed to save default config.proto - private key may be lost on reboot!");
        }
    } else {
        if (config.version < DEVICESTATE_MIN_VER) {
            LOG_WARN("config %d is old, discard", config.version);
            installDefaultConfig(true);  // preserveKey=true to keep existing private key
            // Save updated config after installing defaults (with preserved key)
            LOG_INFO("Saving updated config.proto after version upgrade...");
            if (saveToDisk(SEGMENT_CONFIG)) {
                LOG_INFO("Updated config.proto saved successfully");
            } else {
                LOG_ERROR("Failed to save updated config.proto");
            }
        } else {
            LOG_INFO("Loaded saved config version %d", config.version);
            // CRITICAL: Check if private key was loaded correctly
            if (config.has_security && config.security.private_key.size == 32) {
                LOG_INFO("CRITICAL: Private key loaded successfully from config.proto (32 bytes)");
                // Verify key is not all zeros (low entropy)
                bool is_zero = true;
                for (int i = 0; i < 32; i++) {
                    if (config.security.private_key.bytes[i] != 0) {
                        is_zero = false;
                        break;
                    }
                }
                if (is_zero) {
                    LOG_ERROR("CRITICAL: Private key is all zeros - this is invalid!");
                } else {
                    LOG_DEBUG("Private key is valid (non-zero)");
                }
            } else {
                LOG_WARN("CRITICAL: Private key NOT found in config.proto (size: %d)", 
                         config.has_security ? (int)config.security.private_key.size : 0);
                LOG_WARN("This will cause a new key pair to be generated on next save!");
            }
        }
    }
    if (backupSecurity.private_key.size > 0) {
        LOG_DEBUG("Restoring backup of security config");
        config.security = backupSecurity;
        saveToDisk(SEGMENT_CONFIG);
    }

    // Make sure we load hard coded admin keys even when the configuration file has none.
    // Initialize admin_key_count to zero
    byte numAdminKeys = 0;
#if defined(USERPREFS_USE_ADMIN_KEY_0) || defined(USERPREFS_USE_ADMIN_KEY_1) || defined(USERPREFS_USE_ADMIN_KEY_2)
    uint16_t sum = 0;
#endif
#ifdef USERPREFS_USE_ADMIN_KEY_0

    for (uint8_t b = 0; b < 32; b++) {
        sum += config.security.admin_key[0].bytes[b];
    }
    if (sum == 0) {
        numAdminKeys += 1;
        LOG_INFO("Admin 0 key zero. Loading hard coded key from user preferences.");
        memcpy(config.security.admin_key[0].bytes, userprefs_admin_key_0, 32);
        config.security.admin_key[0].size = 32;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_1
    sum = 0;
    for (uint8_t b = 0; b < 32; b++) {
        sum += config.security.admin_key[1].bytes[b];
    }
    if (sum == 0) {
        numAdminKeys += 1;
        LOG_INFO("Admin 1 key zero. Loading hard coded key from user preferences.");
        memcpy(config.security.admin_key[1].bytes, userprefs_admin_key_1, 32);
        config.security.admin_key[1].size = 32;
    }
#endif

#ifdef USERPREFS_USE_ADMIN_KEY_2
    sum = 0;
    for (uint8_t b = 0; b < 32; b++) {
        sum += config.security.admin_key[2].bytes[b];
    }
    if (sum == 0) {
        numAdminKeys += 1;
        LOG_INFO("Admin 2 key zero. Loading hard coded key from user preferences.");
        memcpy(config.security.admin_key[2].bytes, userprefs_admin_key_2, 32);
        config.security.admin_key[2].size = 32;
    }
#endif

    if (numAdminKeys > 0) {
        LOG_INFO("Saving %d hard coded admin keys.", numAdminKeys);
        config.security.admin_key_count = numAdminKeys;
        saveToDisk(SEGMENT_CONFIG);
    }

    state = loadProto(moduleConfigFileName, meshtastic_LocalModuleConfig_size, sizeof(meshtastic_LocalModuleConfig),
                      &meshtastic_LocalModuleConfig_msg, &moduleConfig);
    if (state != LoadFileResult::LOAD_SUCCESS) {
        LOG_WARN("module.proto not found - installing default module config (this is normal on first boot)");
        installDefaultModuleConfig(); // Our in RAM copy might now be corrupt
        // Save default module config immediately
        LOG_INFO("Saving default module.proto to disk...");
        if (saveToDisk(SEGMENT_MODULECONFIG)) {
            LOG_INFO("Default module.proto saved successfully");
        } else {
            LOG_ERROR("Failed to save default module.proto");
        }
    } else {
        if (moduleConfig.version < DEVICESTATE_MIN_VER) {
            LOG_WARN("moduleConfig %d is old, discard", moduleConfig.version);
            installDefaultModuleConfig();
            // Save updated module config after installing defaults
            LOG_INFO("Saving updated module.proto after version upgrade...");
            if (saveToDisk(SEGMENT_MODULECONFIG)) {
                LOG_INFO("Updated module.proto saved successfully");
            } else {
                LOG_ERROR("Failed to save updated module.proto");
            }
        } else {
            LOG_INFO("Loaded saved moduleConfig version %d", moduleConfig.version);
        }
    }

    state = loadProto(channelFileName, meshtastic_ChannelFile_size, sizeof(meshtastic_ChannelFile), &meshtastic_ChannelFile_msg,
                      &channelFile);
    if (state != LoadFileResult::LOAD_SUCCESS) {
        installDefaultChannels(); // Our in RAM copy might now be corrupt
    } else {
        if (channelFile.version < DEVICESTATE_MIN_VER) {
            LOG_WARN("channelFile %d is old, discard", channelFile.version);
            installDefaultChannels();
        } else {
            LOG_INFO("Loaded saved channelFile version %d", channelFile.version);
        }
    }

    state = loadProto(uiconfigFileName, meshtastic_DeviceUIConfig_size, sizeof(meshtastic_DeviceUIConfig),
                      &meshtastic_DeviceUIConfig_msg, &uiconfig);
    if (state == LoadFileResult::LOAD_SUCCESS) {
        LOG_INFO("Loaded UIConfig");
    }

    // 2.4.X - configuration migration to update new default intervals
    if (moduleConfig.version < 23) {
        LOG_DEBUG("ModuleConfig version %d is stale, upgrading to new default intervals", moduleConfig.version);
        moduleConfig.version = DEVICESTATE_CUR_VER;
        if (moduleConfig.telemetry.device_update_interval == 900)
            moduleConfig.telemetry.device_update_interval = 0;
        if (moduleConfig.telemetry.environment_update_interval == 900)
            moduleConfig.telemetry.environment_update_interval = 0;
        if (moduleConfig.telemetry.air_quality_interval == 900)
            moduleConfig.telemetry.air_quality_interval = 0;
        if (moduleConfig.telemetry.power_update_interval == 900)
            moduleConfig.telemetry.power_update_interval = 0;
        if (moduleConfig.neighbor_info.update_interval == 900)
            moduleConfig.neighbor_info.update_interval = 0;
        if (moduleConfig.paxcounter.paxcounter_update_interval == 900)
            moduleConfig.paxcounter.paxcounter_update_interval = 0;

        saveToDisk(SEGMENT_MODULECONFIG);
    }
}

/** Save a protobuf from a file, return true for success */
bool NodeDB::saveProto(const char *filename, size_t protoSize, const pb_msgdesc_t *fields, const void *dest_struct,
                       bool fullAtomic)
{
    bool okay = false;
#ifdef FSCom
    // Check if this is nodes.proto and if extended filesystem should be used
    #ifdef USE_EXTENDED_FS_FOR_NODEDB
    extern bool useExtendedFSForNodeDB();
    extern bool isNodeDBFile(const char* filename);
    extern void* getExtendedFSForNodeDB();
    
    // IMPORTANT: Only use extended filesystem for nodes.proto (other nodes from network)
    // All other files (config.proto, device.proto, module.proto, channels.proto, uiconfig.proto)
    // MUST go to main filesystem to ensure current node configuration is always saved
    // NO FALLBACK: If extended filesystem fails, nodes.proto simply won't be saved (but system continues)
    if (useExtendedFSForNodeDB() && isNodeDBFile(filename)) {
        // Use extended filesystem for nodes.proto (other nodes from network only)
        LOG_INFO("Save %s to EXTENDED filesystem (80 pages, 320 KB) - other nodes only", filename);
        
        // Cast to lfs_t* (headers already included at top of file)
        // Get fresh pointer each time in case filesystem was reformatted
        lfs_t* extended_lfs = (lfs_t*)getExtendedFSForNodeDB();
        if (extended_lfs) {
            // Reset reformat flag at start of save operation
            static bool reformat_attempted_this_save = false;
            if (reformat_attempted_this_save) {
                reformat_attempted_this_save = false;  // Reset for new save operation
            }
            // CRITICAL: Extended filesystem uses SoftDevice API directly, NOT SPI!
            // Do NOT use writecb here - it uses spiLock for SPI bus (LoRa radio + main filesystem).
            // Use buffer-based encoding instead: encode to buffer, then write buffer to file.
            
            LOG_DEBUG("Opening %s for writing in extended filesystem (buffer-based)...", filename);
            
            // Ensure directory exists (e.g., /prefs/ for /prefs/nodes.proto)
            const char* slash = filename;
            if (slash[0] == '/') {
                slash++; // skip root '/'
            }
            while (NULL != (slash = strchr(slash, '/'))) {
                size_t dir_len = slash - filename;
                char dir_path[64];
                if (dir_len < sizeof(dir_path)) {
                    memcpy(dir_path, filename, dir_len);
                    dir_path[dir_len] = '\0';
                    int mkdir_result = lfs_mkdir(extended_lfs, dir_path);
                    if (mkdir_result != LFS_ERR_OK && mkdir_result != LFS_ERR_EXIST) {
                        LOG_DEBUG("Failed to create directory '%s' in extended FS (error: %d), continuing...", dir_path, mkdir_result);
                    }
                }
                slash++; // move past '/'
            }
            
            // Remove old file first (for atomic write)
            // This is safer than truncate - avoids issues with block allocation
            int remove_result = lfs_remove(extended_lfs, filename);
            if (remove_result == LFS_ERR_OK) {
                LOG_DEBUG("Removed old file '%s' before write", filename);
            } else if (remove_result != LFS_ERR_NOENT) {
                LOG_DEBUG("Failed to remove old file '%s' (error: %d), continuing...", filename, remove_result);
            }
            
            // Open file for writing (create new file)
            lfs_file_t file;
            int flags = LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC;
            int open_result = lfs_file_open(extended_lfs, &file, filename, flags);
            
            if (open_result != LFS_ERR_OK) {
                LOG_ERROR("Failed to open '%s' for writing in extended FS (error: %d)", filename, open_result);
                LOG_ERROR("Filesystem may be full or corrupted - max blocks: 80");
                // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                return false;
            }
            
            LOG_DEBUG("File opened successfully, encoding protobuf to buffer (expected size: %u)...", (unsigned)protoSize);
            
            // Allocate buffer for protobuf encoding (use protoSize as estimate, but add some margin)
            size_t buffer_size = protoSize + 256;  // Add 256 bytes margin for protobuf overhead
            uint8_t* encode_buffer = (uint8_t*)malloc(buffer_size);
            
            if (!encode_buffer) {
                LOG_ERROR("Failed to allocate %u bytes for protobuf encoding", (unsigned)buffer_size);
                lfs_file_close(extended_lfs, &file);
                // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                return false;
            }
            
            // Encode protobuf to buffer (this doesn't use SPI lock)
            pb_ostream_t stream = pb_ostream_from_buffer(encode_buffer, buffer_size);
            
            if (!pb_encode(&stream, fields, dest_struct)) {
                const char* error = PB_GET_ERROR(&stream);
                LOG_ERROR("Error: can't encode protobuf: %s", error ? error : "unknown");
                free(encode_buffer);
                lfs_file_close(extended_lfs, &file);
                // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                return false;
            }
            
            size_t encoded_size = stream.bytes_written;
            LOG_DEBUG("Protobuf encoded successfully: %u bytes", (unsigned)encoded_size);
            
            // Write buffer to file using lfs_file_write
            LOG_DEBUG("Step 3: Writing %u bytes to file '%s'...", (unsigned)encoded_size, filename);
            uint32_t write_start = millis();
            lfs_ssize_t write_result = lfs_file_write(extended_lfs, &file, encode_buffer, encoded_size);
            uint32_t write_time = millis() - write_start;
            
            if (write_result < 0) {
                LOG_ERROR("Step 3 FAILED: Write to '%s' failed (error: %d, took %u ms)", filename, (int)write_result, write_time);
                LOG_ERROR("Failed to write %u bytes to extended FS (error: %d)", (unsigned)encoded_size, (int)write_result);
                LOG_ERROR("Filesystem may be full or corrupted - max blocks: 80");
                
                // Close file before reformat
                lfs_file_close(extended_lfs, &file);
                free(encode_buffer);
                
                // Try to reformat filesystem if error is -5 (LFS_ERR_NOSPC) or other critical errors
                // But only once per save operation to avoid infinite loops
                static bool reformat_attempted_this_save = false;
                if ((write_result == -5 || write_result == -84) && !reformat_attempted_this_save) {
                    reformat_attempted_this_save = true;
                    LOG_WARN("Critical filesystem error detected - attempting reformat...");
                    extern bool forceReformatExtendedFS();
                    if (forceReformatExtendedFS()) {
                        LOG_INFO("Filesystem reformatted successfully - retrying write...");
                        // Get fresh filesystem pointer after reformat
                        extended_lfs = (lfs_t*)getExtendedFSForNodeDB();
                        if (extended_lfs) {
                            // Retry write after reformat (recursive call, but only once)
                            bool retry_result = saveProto(filename, protoSize, fields, dest_struct, fullAtomic);
                            reformat_attempted_this_save = false;  // Reset for next save
                            return retry_result;
                        } else {
                            LOG_ERROR("Failed to get filesystem pointer after reformat - extended filesystem unavailable");
                            reformat_attempted_this_save = false;
                            // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                            return false;
                        }
                    } else {
                        LOG_ERROR("Filesystem reformat failed - extended filesystem unavailable");
                        reformat_attempted_this_save = false;
                        // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                        return false;
                    }
                } else if (reformat_attempted_this_save) {
                    LOG_WARN("Reformat already attempted this save - extended filesystem unavailable");
                    reformat_attempted_this_save = false;
                    // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                    return false;
                } else {
                    // First attempt failed, will try reformat on retry
                    return false;
                }
            } else if ((size_t)write_result != encoded_size) {
                LOG_ERROR("Partial write: expected %u bytes, wrote %d", (unsigned)encoded_size, (int)write_result);
                lfs_file_close(extended_lfs, &file);
                free(encode_buffer);
                // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                return false;
            } else {
                LOG_DEBUG("Step 3 SUCCESS: Wrote %u bytes to file '%s' (took %u ms)", (unsigned)encoded_size, filename, write_time);
                okay = true;
            }
            
            free(encode_buffer);
            
            // Sync file before closing (CRITICAL for LittleFS)
            LOG_DEBUG("Step 4: Syncing file '%s'...", filename);
            uint32_t sync_start = millis();
            int sync_result = lfs_file_sync(extended_lfs, &file);
            uint32_t sync_time = millis() - sync_start;
            if (sync_result != LFS_ERR_OK) {
                LOG_WARN("Step 4 FAILED: Sync of '%s' failed (error: %d, took %u ms)", filename, sync_result, sync_time);
                okay = false;  // Mark as failed if sync fails
            } else {
                LOG_DEBUG("Step 4 SUCCESS: File '%s' synced (took %u ms)", filename, sync_time);
            }
            
            // Close file
            LOG_DEBUG("Step 5: Closing file '%s'...", filename);
            uint32_t close_start = millis();
            int close_result = lfs_file_close(extended_lfs, &file);
            uint32_t close_time = millis() - close_start;
            if (close_result != LFS_ERR_OK) {
                LOG_WARN("Step 5 FAILED: Close of '%s' failed (error: %d, took %u ms)", filename, close_result, close_time);
                okay = false;  // Mark as failed if close fails
            } else {
                LOG_DEBUG("Step 5 SUCCESS: File '%s' closed (took %u ms)", filename, close_time);
            }
            
            if (okay) {
                LOG_INFO("Saved %s successfully to EXTENDED filesystem (%u bytes)", filename, (unsigned)encoded_size);
                
                // Verify file was saved to extended filesystem and NOT to main filesystem
                LOG_DEBUG("VERIFY: Step 1: Checking if '%s' exists in extended filesystem...", filename);
                uint32_t verify_start = millis();
                bool found_in_extended = false;
                lfs_file_t verify_file;
                int verify_result = lfs_file_open(extended_lfs, &verify_file, filename, LFS_O_RDONLY);
                if (verify_result == LFS_ERR_OK) {
                    found_in_extended = true;
                    lfs_soff_t file_size = lfs_file_size(extended_lfs, &verify_file);
                    lfs_file_close(extended_lfs, &verify_file);
                    LOG_DEBUG("VERIFY: Step 1 SUCCESS: File '%s' found in extended FS (size: %d bytes)", filename, (int)file_size);
                } else {
                    LOG_DEBUG("VERIFY: Step 1 FAILED: File '%s' not found in extended FS (error: %d)", filename, verify_result);
                }
                
                LOG_DEBUG("VERIFY: Step 2: Checking if '%s' exists in main filesystem...", filename);
                bool found_in_main = FSCom.exists(filename);
                if (found_in_main) {
                    LOG_DEBUG("VERIFY: Step 2: File '%s' found in main FS (this is WRONG for nodes.proto!)", filename);
                } else {
                    LOG_DEBUG("VERIFY: Step 2 SUCCESS: File '%s' NOT in main FS (correct)", filename);
                }
                
                uint32_t verify_time = millis() - verify_start;
                
                if (found_in_extended && !found_in_main) {
                    LOG_INFO("VERIFIED: %s correctly saved to EXTENDED filesystem (80 pages, 320 KB, verification took %u ms)", 
                            filename, verify_time);
                } else if (found_in_main) {
                    LOG_ERROR("VERIFY FAILED: %s was also found in MAIN filesystem - this should not happen!", filename);
                    LOG_ERROR("nodes.proto should ONLY be in extended filesystem!");
                } else if (!found_in_extended) {
                    LOG_ERROR("VERIFY FAILED: %s not found in EXTENDED filesystem after save (verification took %u ms)!", 
                             filename, verify_time);
                }
                
                return true;
            } else {
                LOG_ERROR("Can't write %s to extended filesystem!", filename);
                // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
                return false;
            }
        } else {
            LOG_WARN("Extended filesystem pointer is null - extended filesystem unavailable");
            // NO FALLBACK: Extended filesystem failed, nodes.proto won't be saved
            return false;
        }
    }
    // Not a NodeDB file or extended FS not enabled - use main filesystem (normal behavior)
    #endif // USE_EXTENDED_FS_FOR_NODEDB
    
    // Use main filesystem (standard behavior) - for all files except nodes.proto
    // config.proto, device.proto, module.proto, channels.proto, uiconfig.proto always go here
    auto f = SafeFile(filename, fullAtomic);

    LOG_INFO("Save %s to MAIN filesystem (7 pages, 28 KB) - current node configuration", filename);
    pb_ostream_t stream = {&writecb, static_cast<Print *>(&f), protoSize};

    if (!pb_encode(&stream, fields, dest_struct)) {
        LOG_ERROR("Error: can't encode protobuf %s", PB_GET_ERROR(&stream));
    } else {
        okay = true;
    }

    bool writeSucceeded = f.close();

    if (!okay || !writeSucceeded) {
        LOG_ERROR("Can't write prefs!");
    } else {
        // Verify file was saved to correct filesystem
        #ifdef USE_EXTENDED_FS_FOR_NODEDB
        extern bool isNodeDBFile(const char* filename);
        extern void* getExtendedFSForNodeDB();
        
        bool should_be_in_extended = isNodeDBFile(filename);
        
        LOG_DEBUG("VERIFY: Checking filesystem location for '%s' (should be in %s)...", 
                 filename, should_be_in_extended ? "EXTENDED" : "MAIN");
        uint32_t verify_start = millis();
        
        bool found_in_main = FSCom.exists(filename);
        LOG_DEBUG("VERIFY: Main filesystem check: %s", found_in_main ? "FOUND" : "NOT FOUND");
        
        lfs_t* extended_lfs = (lfs_t*)getExtendedFSForNodeDB();
        bool found_in_extended = false;
        if (extended_lfs) {
            lfs_file_t test_file;
            int open_result = lfs_file_open(extended_lfs, &test_file, filename, LFS_O_RDONLY);
            if (open_result == LFS_ERR_OK) {
                found_in_extended = true;
                lfs_soff_t file_size = lfs_file_size(extended_lfs, &test_file);
                lfs_file_close(extended_lfs, &test_file);
                LOG_DEBUG("VERIFY: Extended filesystem check: FOUND (size: %d bytes)", (int)file_size);
            } else {
                LOG_DEBUG("VERIFY: Extended filesystem check: NOT FOUND (error: %d)", open_result);
            }
        } else {
            LOG_DEBUG("VERIFY: Extended filesystem not available for check");
        }
        
        uint32_t verify_time = millis() - verify_start;
        
        if (should_be_in_extended) {
            // nodes.proto should be in extended filesystem
            if (found_in_extended && !found_in_main) {
                LOG_INFO("VERIFIED: %s correctly saved to EXTENDED filesystem (80 pages, 320 KB, verification took %u ms)", 
                        filename, verify_time);
            } else if (found_in_main) {
                LOG_ERROR("VERIFY FAILED: %s was saved to MAIN filesystem but should be in EXTENDED!", filename);
                LOG_ERROR("This is a configuration error - nodes.proto must be in extended filesystem!");
            } else if (!found_in_extended) {
                LOG_ERROR("VERIFY FAILED: %s not found in EXTENDED filesystem after save (verification took %u ms)!", 
                         filename, verify_time);
            }
        } else {
            // config.proto, device.proto, etc. should be in main filesystem
            if (found_in_main && !found_in_extended) {
                LOG_INFO("VERIFIED: %s correctly saved to MAIN filesystem (7 pages, 28 KB, verification took %u ms)", 
                        filename, verify_time);
                
                // CRITICAL: For config.proto, verify private key is saved
                if (strcmp(filename, "/prefs/config.proto") == 0) {
                    LOG_DEBUG("VERIFY KEY: Checking private key in config.proto...");
                    // Check if config contains private key (config is global variable)
                    extern meshtastic_LocalConfig config;
                    if (config.has_security && config.security.private_key.size == 32) {
                        // Verify key is not all zeros
                        bool is_zero = true;
                        uint8_t non_zero_count = 0;
                        for (int i = 0; i < 32; i++) {
                            if (config.security.private_key.bytes[i] != 0) {
                                is_zero = false;
                                non_zero_count++;
                            }
                        }
                        if (is_zero) {
                            LOG_ERROR("VERIFY KEY FAILED: Private key in config.proto is all zeros - this is invalid!");
                        } else {
                            LOG_INFO("VERIFY KEY SUCCESS: Private key verified in config.proto (32 bytes, %u non-zero bytes) - key will persist across reboots", 
                                    non_zero_count);
                            // Log first 4 bytes for debugging (not security risk - just for verification)
                            LOG_DEBUG("VERIFY KEY: First 4 bytes of private key: %02X %02X %02X %02X", 
                                     config.security.private_key.bytes[0],
                                     config.security.private_key.bytes[1],
                                     config.security.private_key.bytes[2],
                                     config.security.private_key.bytes[3]);
                        }
                    } else {
                        LOG_ERROR("VERIFY KEY FAILED: Private key NOT found in saved config.proto!");
                        LOG_ERROR("  - has_security: %d", config.has_security ? 1 : 0);
                        LOG_ERROR("  - private_key.size: %d", config.has_security ? (int)config.security.private_key.size : 0);
                        LOG_ERROR("This will cause a new key pair to be generated on next reboot!");
                    }
                }
            } else if (found_in_extended) {
                LOG_ERROR("ERROR: %s was saved to EXTENDED filesystem but should be in MAIN!", filename);
                LOG_ERROR("This is a configuration error - current node config must be in main filesystem!");
                if (strcmp(filename, "/prefs/config.proto") == 0) {
                    LOG_ERROR("CRITICAL: config.proto with private key is in wrong filesystem!");
                }
            } else if (!found_in_main) {
                LOG_ERROR("ERROR: %s not found in MAIN filesystem after save!", filename);
                if (strcmp(filename, "/prefs/config.proto") == 0) {
                    LOG_ERROR("CRITICAL: config.proto with private key was not saved!");
                }
            }
        }
        #endif // USE_EXTENDED_FS_FOR_NODEDB
    }
#else
    LOG_ERROR("ERROR: Filesystem not implemented");
#endif
    return okay;
}

bool NodeDB::saveChannelsToDisk()
{
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    return saveProto(channelFileName, meshtastic_ChannelFile_size, &meshtastic_ChannelFile_msg, &channelFile);
}

bool NodeDB::saveDeviceStateToDisk()
{
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    return saveProto(deviceStateFileName, meshtastic_DeviceState_size, &meshtastic_DeviceState_msg, &devicestate, false);
}bool NodeDB::saveNodeDatabaseToDisk()
{
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    size_t nodeDatabaseSize;
    pb_get_encoded_size(&nodeDatabaseSize, meshtastic_NodeDatabase_fields, &nodeDatabase);

    // Log database size for monitoring large node databases
    if (nodeDatabaseSize > 50000 || numMeshNodes > 100) {
        LOG_INFO("Saving NodeDB: %d nodes, %u bytes encoded, free heap: %u bytes",
                 numMeshNodes, nodeDatabaseSize, memGet.getFreeHeap());
    }

    bool saveResult = saveProto(nodeDatabaseFileName, nodeDatabaseSize, &meshtastic_NodeDatabase_msg, &nodeDatabase, false);

    // DISABLED: Verification loads entire DB into RAM (125KB!)
    // This causes OOM crash on devices with 248KB RAM
    // TODO: Implement streaming verification without loading full DB
    // if (saveResult && (nodeDatabaseSize > 50000 || numMeshNodes > 100)) {
    //     verifyNodeDatabaseFromDisk();
    // }

    return saveResult;
}

// Verify saved database by reading from disk and logging all nodes
void NodeDB::verifyNodeDatabaseFromDisk()
{
#ifdef FSCom
    concurrency::LockGuard g(spiLock);

    auto f = FSCom.open(nodeDatabaseFileName, FILE_O_READ);
    if (!f) {
        LOG_ERROR("Failed to open %s for verification", nodeDatabaseFileName);
        return;
    }

    size_t fileSize = f.size();
    LOG_INFO("Verifying NodeDB from flash: file size = %u bytes", fileSize);

    // Read and decode the database from flash
    meshtastic_NodeDatabase verifyDb = {};
    verifyDb.version = 0;
    pb_istream_t stream = {&readcb, &f, fileSize};

    if (!pb_decode(&stream, &meshtastic_NodeDatabase_msg, &verifyDb)) {
        LOG_ERROR("Failed to decode NodeDB from flash: %s", PB_GET_ERROR(&stream));
        f.close();
        return;
    }

    f.close();

    // Count valid nodes in saved database
    int validNodes = 0;
    for (size_t i = 0; i < verifyDb.nodes.size(); i++) {
        if (verifyDb.nodes[i].has_user && verifyDb.nodes[i].num != 0) {
            validNodes++;
        }
    }

    LOG_INFO("✓ NodeDB verified from flash: %d valid nodes out of %d total",
             validNodes, verifyDb.nodes.size());

    // Log first 20 nodes as sample (to avoid flooding logs with 500 nodes)
    LOG_INFO("Sample of saved nodes (first 20):");
    int logged = 0;
    for (size_t i = 0; i < verifyDb.nodes.size() && logged < 20; i++) {
        if (verifyDb.nodes[i].has_user && verifyDb.nodes[i].num != 0) {
            LOG_INFO("  Node[%d]: 0x%08x %s/%s",
                     logged + 1,
                     verifyDb.nodes[i].num,
                     verifyDb.nodes[i].user.long_name,
                     verifyDb.nodes[i].user.short_name);
            logged++;
        }
    }

    if (validNodes > 20) {
        LOG_INFO("  ... and %d more nodes", validNodes - 20);
    }
#endif
}

bool NodeDB::saveToDiskNoRetry(int saveWhat)
{
    bool success = true;
#ifdef FSCom
    spiLock->lock();
    FSCom.mkdir("/prefs");
    spiLock->unlock();
#endif
    if (saveWhat & SEGMENT_CONFIG) {
        config.has_device = true;
        config.has_display = true;
        config.has_lora = true;
        config.has_position = true;
        config.has_power = true;
        config.has_network = true;
        config.has_bluetooth = true;
        config.has_security = true;

        success &= saveProto(configFileName, meshtastic_LocalConfig_size, &meshtastic_LocalConfig_msg, &config);
    }

    if (saveWhat & SEGMENT_MODULECONFIG) {
        moduleConfig.has_canned_message = true;
        moduleConfig.has_external_notification = true;
        moduleConfig.has_mqtt = true;
        moduleConfig.has_range_test = true;
        moduleConfig.has_serial = true;
        moduleConfig.has_store_forward = true;
        moduleConfig.has_telemetry = true;
        moduleConfig.has_neighbor_info = true;
        moduleConfig.has_detection_sensor = true;
        moduleConfig.has_ambient_lighting = true;
        moduleConfig.has_audio = true;
        moduleConfig.has_paxcounter = true;

        success &=
            saveProto(moduleConfigFileName, meshtastic_LocalModuleConfig_size, &meshtastic_LocalModuleConfig_msg, &moduleConfig);
    }

    if (saveWhat & SEGMENT_CHANNELS) {
        success &= saveChannelsToDisk();
    }

    if (saveWhat & SEGMENT_DEVICESTATE) {
        success &= saveDeviceStateToDisk();
    }

    if (saveWhat & SEGMENT_NODEDATABASE) {
        success &= saveNodeDatabaseToDisk();
    }

    return success;
}

bool NodeDB::saveToDisk(int saveWhat)
{
    LOG_DEBUG("Save to disk %d", saveWhat);
    bool success = saveToDiskNoRetry(saveWhat);

    if (!success) {
        LOG_ERROR("Failed to save to disk, retrying");
#ifdef ARCH_NRF52 // @geeksville is not ready yet to say we should do this on other platforms.  See bug #4184 discussion
        spiLock->lock();
        FSCom.format();
        spiLock->unlock();

#endif
        success = saveToDiskNoRetry(saveWhat);

        RECORD_CRITICALERROR(success ? meshtastic_CriticalErrorCode_FLASH_CORRUPTION_RECOVERABLE
                                     : meshtastic_CriticalErrorCode_FLASH_CORRUPTION_UNRECOVERABLE);
    }

    return success;
}

const meshtastic_NodeInfoLite *NodeDB::readNextMeshNode(uint32_t &readIndex)
{
    if (readIndex < numMeshNodes)
        return &meshNodes->at(readIndex++);
    else
        return NULL;
}

/// Given a node, return how many seconds in the past (vs now) that we last heard from it
uint32_t sinceLastSeen(const meshtastic_NodeInfoLite *n)
{
    uint32_t now = getTime();

    int delta = (int)(now - n->last_heard);
    if (delta < 0) // our clock must be slightly off still - not set from GPS yet
        delta = 0;

    return delta;
}

uint32_t sinceReceived(const meshtastic_MeshPacket *p)
{
    uint32_t now = getTime();

    int delta = (int)(now - p->rx_time);
    if (delta < 0) // our clock must be slightly off still - not set from GPS yet
        delta = 0;

    return delta;
}

#define NUM_ONLINE_SECS (60 * 60 * 2) // 2 hrs to consider someone offline

size_t NodeDB::getNumOnlineMeshNodes(bool localOnly)
{
    size_t numseen = 0;

    // FIXME this implementation is kinda expensive
    for (int i = 0; i < numMeshNodes; i++) {
        if (localOnly && meshNodes->at(i).via_mqtt)
            continue;
        if (sinceLastSeen(&meshNodes->at(i)) < NUM_ONLINE_SECS)
            numseen++;
    }

    return numseen;
}

#include "MeshModule.h"
#include "Throttle.h"

/** Update position info for this node based on received position data
 */
void NodeDB::updatePosition(uint32_t nodeId, const meshtastic_Position &p, RxSource src)
{
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(nodeId);
    if (!info) {
        return;
    }

    if (src == RX_SRC_LOCAL) {
        // Local packet, fully authoritative
        LOG_INFO("updatePosition LOCAL pos@%x time=%u lat=%d lon=%d alt=%d", p.timestamp, p.time, p.latitude_i, p.longitude_i,
                 p.altitude);

        setLocalPosition(p);
        info->position = TypeConversions::ConvertToPositionLite(p);
    } else if ((p.time > 0) && !p.latitude_i && !p.longitude_i && !p.timestamp && !p.location_source) {
        // FIXME SPECIAL TIME SETTING PACKET FROM EUD TO RADIO
        // (stop-gap fix for issue #900)
        LOG_DEBUG("updatePosition SPECIAL time setting time=%u", p.time);
        info->position.time = p.time;
    } else {
        // Be careful to only update fields that have been set by the REMOTE sender
        // A lot of position reports don't have time populated.  In that case, be careful to not blow away the time we
        // recorded based on the packet rxTime
        //
        // FIXME perhaps handle RX_SRC_USER separately?
        LOG_INFO("updatePosition REMOTE node=0x%x time=%u lat=%d lon=%d", nodeId, p.time, p.latitude_i, p.longitude_i);

        // First, back up fields that we want to protect from overwrite
        uint32_t tmp_time = info->position.time;

        // Next, update atomically
        info->position = TypeConversions::ConvertToPositionLite(p);

        // Last, restore any fields that may have been overwritten
        if (!info->position.time)
            info->position.time = tmp_time;
    }
    info->has_position = true;
    updateGUIforNode = info;
    notifyObservers(true); // Force an update whether or not our node counts have changed
}

/** Update telemetry info for this node based on received metrics
 *  We only care about device telemetry here
 */
void NodeDB::updateTelemetry(uint32_t nodeId, const meshtastic_Telemetry &t, RxSource src)
{
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(nodeId);
    // Environment metrics should never go to NodeDb but we'll safegaurd anyway
    if (!info || t.which_variant != meshtastic_Telemetry_device_metrics_tag) {
        return;
    }

    if (src == RX_SRC_LOCAL) {
        // Local packet, fully authoritative
        LOG_DEBUG("updateTelemetry LOCAL");
    } else {
        LOG_DEBUG("updateTelemetry REMOTE node=0x%x ", nodeId);
    }
    info->device_metrics = t.variant.device_metrics;
    info->has_device_metrics = true;
    updateGUIforNode = info;
    notifyObservers(true); // Force an update whether or not our node counts have changed
}

/**
 * Update the node database with a new contact
 */
void NodeDB::addFromContact(meshtastic_SharedContact contact)
{
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(contact.node_num);
    if (!info) {
        return;
    }
    info->num = contact.node_num;
    info->last_heard = getValidTime(RTCQualityNTP);
    info->has_user = true;
    info->user = TypeConversions::ConvertToUserLite(contact.user);
    info->is_favorite = true;
    // Mark the node's key as manually verified to indicate trustworthiness.
    info->bitfield |= NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK;
    updateGUIforNode = info;
    powerFSM.trigger(EVENT_NODEDB_UPDATED);
    notifyObservers(true); // Force an update whether or not our node counts have changed
    saveNodeDatabaseToDisk();
}

/** Update user info and channel for this node based on received user data
 */
bool NodeDB::updateUser(uint32_t nodeId, meshtastic_User &p, uint8_t channelIndex)
{
    meshtastic_NodeInfoLite *info = getOrCreateMeshNode(nodeId);
    if (!info) {
        return false;
    }

#if !(MESHTASTIC_EXCLUDE_PKI)
    if (p.public_key.size == 32) {
        printBytes("Incoming Pubkey: ", p.public_key.bytes, 32);

        // Alert the user if a remote node is advertising public key that matches our own
        if (owner.public_key.size == 32 && memcmp(p.public_key.bytes, owner.public_key.bytes, 32) == 0 && !duplicateWarned) {
            duplicateWarned = true;
            char warning[] = "Remote device %s has advertised your public key. This may indicate a low-entropy key. You may need "
                             "to regenerate your public keys.";
            LOG_WARN(warning, p.long_name);
            meshtastic_ClientNotification *cn = clientNotificationPool.allocZeroed();
            cn->level = meshtastic_LogRecord_Level_WARNING;
            cn->time = getValidTime(RTCQualityFromNet);
            sprintf(cn->message, warning, p.long_name);
            service->sendClientNotification(cn);
        }
    }
    if (info->user.public_key.size > 0) { // if we have a key for this user already, don't overwrite with a new one
        LOG_INFO("Public Key set for node, not updating!");
        // we copy the key into the incoming packet, to prevent overwrite
        p.public_key.size = 32;
        memcpy(p.public_key.bytes, info->user.public_key.bytes, 32);
    } else if (p.public_key.size > 0) {
        LOG_INFO("Update Node Pubkey!");
    }
#endif

    // Both of info->user and p start as filled with zero so I think this is okay
    auto lite = TypeConversions::ConvertToUserLite(p);
    bool changed = memcmp(&info->user, &lite, sizeof(info->user)) || (info->channel != channelIndex);

    info->user = lite;
    if (info->user.public_key.size == 32) {
        printBytes("Saved Pubkey: ", info->user.public_key.bytes, 32);
    }
    if (nodeId != getNodeNum())
        info->channel = channelIndex; // Set channel we need to use to reach this node (but don't set our own channel)
    LOG_DEBUG("Update changed=%d user %s/%s, id=0x%08x, channel=%d", changed, info->user.long_name, info->user.short_name, nodeId,
              info->channel);
    info->has_user = true;

    if (changed) {
        updateGUIforNode = info;
        powerFSM.trigger(EVENT_NODEDB_UPDATED);
        notifyObservers(true); // Force an update whether or not our node counts have changed

        // We just changed something about a User,
        // store our DB unless we just did so less than a minute ago

        if (!Throttle::isWithinTimespanMs(lastNodeDbSave, ONE_MINUTE_MS)) {
            saveToDisk(SEGMENT_NODEDATABASE);
            lastNodeDbSave = millis();
        } else {
            LOG_DEBUG("Defer NodeDB saveToDisk for now");
        }
    }

    return changed;
}

/// given a subpacket sniffed from the network, update our DB state
/// we updateGUI and updateGUIforNode if we think our this change is big enough for a redraw
void NodeDB::updateFrom(const meshtastic_MeshPacket &mp)
{
    // if (mp.from == getNodeNum()) {
    //     LOG_DEBUG("Ignore update from self");
    //     return;
    // }
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag && mp.from) {
        LOG_DEBUG("Update DB node 0x%x, rx_time=%u", mp.from, mp.rx_time);

        meshtastic_NodeInfoLite *info = getOrCreateMeshNode(getFrom(&mp));
        if (!info) {
            return;
        }

        if (mp.rx_time) // if the packet has a valid timestamp use it to update our last_heard
            info->last_heard = mp.rx_time;

        if (mp.rx_snr)
            info->snr = mp.rx_snr; // keep the most recent SNR we received for this node.

        info->via_mqtt = mp.via_mqtt; // Store if we received this packet via MQTT

        // If hopStart was set and there wasn't someone messing with the limit in the middle, add hopsAway
        if (mp.hop_start != 0 && mp.hop_limit <= mp.hop_start) {
            info->has_hops_away = true;
            info->hops_away = mp.hop_start - mp.hop_limit;
        }
    }
}

uint8_t NodeDB::getMeshNodeChannel(NodeNum n)
{
    const meshtastic_NodeInfoLite *info = getMeshNode(n);
    if (!info) {
        return 0; // defaults to PRIMARY
    }
    return info->channel;
}

/// Find a node in our DB, return null for missing
/// NOTE: This function might be called from an ISR
meshtastic_NodeInfoLite *NodeDB::getMeshNode(NodeNum n)
{
    for (int i = 0; i < numMeshNodes; i++)
        if (meshNodes->at(i).num == n)
            return &meshNodes->at(i);

    return NULL;
}

// returns true if we are running low on memory
// Note: Node count limit is checked separately in getOrCreateMeshNode() using MAX_NUM_NODES
bool NodeDB::isFull()
{
    return (memGet.getFreeHeap() < MINIMUM_SAFE_FREE_HEAP);
}

/// Find a node in our DB, create an empty NodeInfo if missing
meshtastic_NodeInfoLite *NodeDB::getOrCreateMeshNode(NodeNum n)
{
    meshtastic_NodeInfoLite *lite = getMeshNode(n);

    if (!lite) {
        // Check node limit and memory constraints
        bool reachedNodeLimit = (numMeshNodes >= MAX_NUM_NODES);
        bool lowMemory = isFull();

        if (reachedNodeLimit || lowMemory) {
            if (reachedNodeLimit) {
                LOG_INFO("Node database full: %u/%u nodes", numMeshNodes, MAX_NUM_NODES);
            }
            if (lowMemory) {
                LOG_WARN("Low memory: %u bytes free (minimum: %u)",
                         memGet.getFreeHeap(), MINIMUM_SAFE_FREE_HEAP);
            }
            LOG_INFO("Erasing oldest entry");
            // look for oldest node and erase it
            uint32_t oldest = UINT32_MAX;
            uint32_t oldestBoring = UINT32_MAX;
            int oldestIndex = -1;
            int oldestBoringIndex = -1;
            for (int i = 1; i < numMeshNodes; i++) {
                // Simply the oldest non-favorite, non-ignored, non-verified node
                if (!meshNodes->at(i).is_favorite && !meshNodes->at(i).is_ignored &&
                    !(meshNodes->at(i).bitfield & NODEINFO_BITFIELD_IS_KEY_MANUALLY_VERIFIED_MASK) &&
                    meshNodes->at(i).last_heard < oldest) {
                    oldest = meshNodes->at(i).last_heard;
                    oldestIndex = i;
                }
                // The oldest "boring" node
                if (!meshNodes->at(i).is_favorite && !meshNodes->at(i).is_ignored && meshNodes->at(i).user.public_key.size == 0 &&
                    meshNodes->at(i).last_heard < oldestBoring) {
                    oldestBoring = meshNodes->at(i).last_heard;
                    oldestBoringIndex = i;
                }
            }
            // if we found a "boring" node, evict it
            if (oldestBoringIndex != -1) {
                oldestIndex = oldestBoringIndex;
            }

            if (oldestIndex != -1) {
                // Shove the remaining nodes down the chain
                for (int i = oldestIndex; i < numMeshNodes - 1; i++) {
                    meshNodes->at(i) = meshNodes->at(i + 1);
                }
                (numMeshNodes)--;
            }
        }

        // CRITICAL FIX: Use push_back() instead of at() since we only reserve capacity, not size
        // Create new node and add it to vector
        meshtastic_NodeInfoLite newNode = {};
        newNode.num = n;
        meshNodes->push_back(newNode);
        numMeshNodes++;
        lite = &meshNodes->at(numMeshNodes - 1);
        LOG_INFO("Adding node to database with %i nodes and %u bytes free!", numMeshNodes, memGet.getFreeHeap());
    }

    return lite;
}

/// Sometimes we will have Position objects that only have a time, so check for
/// valid lat/lon
bool NodeDB::hasValidPosition(const meshtastic_NodeInfoLite *n)
{
    return n->has_position && (n->position.latitude_i != 0 || n->position.longitude_i != 0);
}

/// If we have a node / user and they report is_licensed = true
/// we consider them licensed
UserLicenseStatus NodeDB::getLicenseStatus(uint32_t nodeNum)
{
    meshtastic_NodeInfoLite *info = getMeshNode(nodeNum);
    if (!info || !info->has_user) {
        return UserLicenseStatus::NotKnown;
    }
    return info->user.is_licensed ? UserLicenseStatus::Licensed : UserLicenseStatus::NotLicensed;
}

bool NodeDB::checkLowEntropyPublicKey(const meshtastic_User_public_key_t keyToTest)
{
    uint8_t keyHash[32] = {0};
    memcpy(keyHash, keyToTest.bytes, keyToTest.size);
    crypto->hash(keyHash, 32);
    if (memcmp(keyHash, LOW_ENTROPY_HASH1, sizeof(LOW_ENTROPY_HASH1)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH2, sizeof(LOW_ENTROPY_HASH2)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH3, sizeof(LOW_ENTROPY_HASH3)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH4, sizeof(LOW_ENTROPY_HASH4)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH5, sizeof(LOW_ENTROPY_HASH5)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH6, sizeof(LOW_ENTROPY_HASH6)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH7, sizeof(LOW_ENTROPY_HASH7)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH8, sizeof(LOW_ENTROPY_HASH8)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH9, sizeof(LOW_ENTROPY_HASH9)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH10, sizeof(LOW_ENTROPY_HASH10)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH11, sizeof(LOW_ENTROPY_HASH11)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH12, sizeof(LOW_ENTROPY_HASH12)) == 0 ||
        memcmp(keyHash, LOW_ENTROPY_HASH13, sizeof(LOW_ENTROPY_HASH13)) == 0) {
        return true;
    } else {
        return false;
    }
}

bool NodeDB::backupPreferences(meshtastic_AdminMessage_BackupLocation location)
{
    bool success = false;
    lastBackupAttempt = millis();
#ifdef FSCom
    if (location == meshtastic_AdminMessage_BackupLocation_FLASH) {
        meshtastic_BackupPreferences backup = meshtastic_BackupPreferences_init_zero;
        backup.version = DEVICESTATE_CUR_VER;
        backup.timestamp = getValidTime(RTCQuality::RTCQualityDevice, false);
        backup.has_config = true;
        backup.config = config;
        backup.has_module_config = true;
        backup.module_config = moduleConfig;
        backup.has_channels = true;
        backup.channels = channelFile;
        backup.has_owner = true;
        backup.owner = owner;

        size_t backupSize;
        pb_get_encoded_size(&backupSize, meshtastic_BackupPreferences_fields, &backup);

        spiLock->lock();
        FSCom.mkdir("/backups");
        spiLock->unlock();
        success = saveProto(backupFileName, backupSize, &meshtastic_BackupPreferences_msg, &backup);

        if (success) {
            LOG_INFO("Saved backup preferences");
        } else {
            LOG_ERROR("Failed to save backup preferences to file");
        }
    } else if (location == meshtastic_AdminMessage_BackupLocation_SD) {
        // TODO: After more mainline SD card support
    }
#endif
    return success;
}

bool NodeDB::restorePreferences(meshtastic_AdminMessage_BackupLocation location, int restoreWhat)
{
    bool success = false;
#ifdef FSCom
    if (location == meshtastic_AdminMessage_BackupLocation_FLASH) {
        spiLock->lock();
        if (!FSCom.exists(backupFileName)) {
            spiLock->unlock();
            LOG_WARN("Could not restore. No backup file found");
            return false;
        } else {
            spiLock->unlock();
        }
        meshtastic_BackupPreferences backup = meshtastic_BackupPreferences_init_zero;
        success = loadProto(backupFileName, meshtastic_BackupPreferences_size, sizeof(meshtastic_BackupPreferences),
                            &meshtastic_BackupPreferences_msg, &backup);
        if (success) {
            if (restoreWhat & SEGMENT_CONFIG) {
                config = backup.config;
                LOG_DEBUG("Restored config");
            }
            if (restoreWhat & SEGMENT_MODULECONFIG) {
                moduleConfig = backup.module_config;
                LOG_DEBUG("Restored module config");
            }
            if (restoreWhat & SEGMENT_DEVICESTATE) {
                devicestate.owner = backup.owner;
                LOG_DEBUG("Restored device state");
            }
            if (restoreWhat & SEGMENT_CHANNELS) {
                channelFile = backup.channels;
                LOG_DEBUG("Restored channels");
            }

            success = saveToDisk(restoreWhat);
            if (success) {
                LOG_INFO("Restored preferences from backup");
            } else {
                LOG_ERROR("Failed to save restored preferences to flash");
            }
        } else {
            LOG_ERROR("Failed to restore preferences from backup file");
        }
    } else if (location == meshtastic_AdminMessage_BackupLocation_SD) {
        // TODO: After more mainline SD card support
    }
    return success;
#endif
}

/// Record an error that should be reported via analytics
void recordCriticalError(meshtastic_CriticalErrorCode code, uint32_t address, const char *filename)
{
    // Print error to screen and serial port
    String lcd = String("Critical error ") + code + "!\n";
    if (screen)
        screen->print(lcd.c_str());
    if (filename) {
        LOG_ERROR("NOTE! Record critical error %d at %s:%lu", code, filename, address);
    } else {
        LOG_ERROR("NOTE! Record critical error %d, address=0x%lx", code, address);
    }

    // Record error to DB
    error_code = code;
    error_address = address;

    // Currently portuino is mostly used for simulation.  Make sure the user notices something really bad happened
#ifdef ARCH_PORTDUINO
    LOG_ERROR("A critical failure occurred, portduino is exiting");
    exit(2);
#endif
}
