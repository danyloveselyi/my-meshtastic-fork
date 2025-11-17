#include "NodeStats.h"
#include "NodeDB.h"
#include "mesh-pb-constants.h"

size_t NodeStats::getValidNodeCount(NodeDB* nodeDB)
{
    if (!nodeDB || !nodeDB->meshNodes) {
        return 0;
    }

    size_t count = 0;
    size_t numNodes = nodeDB->getNumMeshNodes();

    // Limit iteration to prevent runaway loops
    size_t maxIterations = (numNodes < MAX_NUM_NODES) ? numNodes : MAX_NUM_NODES;

    for (size_t i = 0; i < maxIterations; i++) {
        if (nodeDB->meshNodes->at(i).has_user) {
            count++;
        }
    }

    return count;
}

size_t NodeStats::getTotalNodeCount(NodeDB* nodeDB)
{
    if (!nodeDB) {
        return 0;
    }

    return nodeDB->getNumMeshNodes();
}
