#pragma once

#include <cstdint>

// Forward declaration
class NodeDB;

/**
 * @brief Self-contained node database statistics
 *
 * Provides node counting and statistics using only public NodeDB interfaces.
 */
class NodeStats {
public:
    /**
     * @brief Count valid nodes (nodes with user data)
     * @param nodeDB NodeDB instance to query
     * @return Number of valid nodes
     */
    static size_t getValidNodeCount(NodeDB* nodeDB);

    /**
     * @brief Get total node slots used
     * @param nodeDB NodeDB instance to query
     * @return Total number of node slots
     */
    static size_t getTotalNodeCount(NodeDB* nodeDB);
};
