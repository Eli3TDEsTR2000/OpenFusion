#include "CNProtocol.hpp"
#include "CNStructs.hpp"
#include "CNShardServer.hpp"
#include "PlayerManager.hpp"
#include "MobManager.hpp"
#include "CNShared.hpp"
#include "settings.hpp"
#include "Database.hpp"
#include "TableData.hpp" // for flush()

#include <iostream>
#include <sstream>
#include <cstdlib>

std::map<uint32_t, PacketHandler> CNShardServer::ShardPackets;
std::list<TimerEvent> CNShardServer::Timers;

CNShardServer::CNShardServer(uint16_t p) {
    port = p;
    pHandler = &CNShardServer::handlePacket;
    REGISTER_SHARD_TIMER(keepAliveTimer, 4000);
    REGISTER_SHARD_TIMER(periodicSaveTimer, settings::DBSAVEINTERVAL*1000);
    init();
}

void CNShardServer::handlePacket(CNSocket* sock, CNPacketData* data) {
    printPacket(data, CL2FE);

    if (ShardPackets.find(data->type) != ShardPackets.end())
        ShardPackets[data->type](sock, data);
    else if (settings::VERBOSITY > 0)
        std::cerr << "OpenFusion: SHARD UNIMPLM ERR. PacketType: " << Defines::p2str(CL2FE, data->type) << " (" << data->type << ")" << std::endl;

    PlayerManager::players[sock].lastHeartbeat = getTime();
}

void CNShardServer::keepAliveTimer(CNServer* serv, time_t currTime) {
    for (auto& pair : PlayerManager::players) {
        if (pair.second.lastHeartbeat != 0 && currTime - pair.second.lastHeartbeat > settings::TIMEOUT) {
            // if the client hasn't responded in 60 seconds, its a dead connection so throw it out
            pair.first->kill();
        } else if (pair.second.lastHeartbeat != 0 && currTime - pair.second.lastHeartbeat > settings::TIMEOUT/2) {
            // if the player hasn't responded in 30 seconds, send a live check
            INITSTRUCT(sP_FE2CL_REQ_LIVE_CHECK, data);
            pair.first->sendPacket((void*)&data, P_FE2CL_REQ_LIVE_CHECK, sizeof(sP_FE2CL_REQ_LIVE_CHECK));
        }
    }
}

void CNShardServer::periodicSaveTimer(CNServer* serv, time_t currTime) {
    if (PlayerManager::players.empty())
        return;

    std::cout << "[INFO] Saving " << PlayerManager::players.size() << " players to DB..." << std::endl;

    for (auto& pair : PlayerManager::players) {
        Database::updatePlayer(pair.second.plr);
    }

    TableData::flush();
    std::cout << "[INFO] Done." << std::endl;
}

void CNShardServer::newConnection(CNSocket* cns) {
    cns->setActiveKey(SOCKETKEY_E); // by default they accept keys encrypted with the default key
}

// must be static to be called from PlayerManager::exitDuplicate()
void CNShardServer::_killConnection(CNSocket* cns) {
    // check if the player ever sent a REQ_PC_ENTER
    if (PlayerManager::players.find(cns) == PlayerManager::players.end())
        return;

    Player* plr = PlayerManager::getPlayer(cns);

    if (plr == nullptr) { // this shouldn't happen if everything works correctly...
        PlayerManager::removePlayer(cns);

        // also, hopefully the player's progress was already saved since the last db save interval, but rip those 2 mins of progress lol
        return;
    }

    int64_t key = plr->SerialKey;

    PlayerManager::removePlayer(cns); // removes the player from the list and saves it to DB

    // remove from CNSharedData
    CNSharedData::erasePlayer(key);
}

void CNShardServer::killConnection(CNSocket *cns) {
    _killConnection(cns);
}

// flush the DB when terminating the server
void CNShardServer::kill() {
    periodicSaveTimer(nullptr, 0);
    CNServer::kill();
}

void CNShardServer::onStep() {
    time_t currTime = getTime();

    for (TimerEvent& event : Timers) {
        if (event.scheduledEvent == 0) {
            // event hasn't been queued yet, go ahead and do that
            event.scheduledEvent = currTime + event.delta;
            continue;
        }

        if (event.scheduledEvent < currTime) {
            // timer needs to be called
            event.handlr(this, currTime);
            event.scheduledEvent = currTime + event.delta;
        }
    }
}
