// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Curve25519.h"
#include "lib/catch.hpp"
#include "main/CommandHandler.h"
#include "overlay/SurveyManager.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace stellar;

TEST_CASE("topology encrypted response memory check",
          "[overlay][survey][topology]")
{
    SurveyResponseBody body;
    body.type(SURVEY_TOPOLOGY);

    auto& topologyBody = body.topologyResponseBody();

    // Fill up the PeerStatLists
    for (uint32_t i = 0; i < PeerStatList::max_size(); ++i)
    {
        PeerStats s;
        s.versionStr = std::string(s.versionStr.max_size(), 'a');
        topologyBody.inboundPeers.push_back(s);
        topologyBody.outboundPeers.push_back(s);
    }

    auto publicKey = curve25519DerivePublic(curve25519RandomSecret());
    // this will throw if EncryptedBody is too small
    curve25519Encrypt<EncryptedBody::max_size()>(publicKey,
                                                 xdr::xdr_to_opaque(body));
}

TEST_CASE("topology survey", "[overlay][survey][topology]")
{
    enum
    {
        A,
        B,
        C,
        D, // older overlay version
        E, // not in transitive quorum
        F
    };

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    std::vector<Config> configList;
    std::vector<PublicKey> keyList;
    std::vector<std::string> keyStrList;
    for (int i = A; i <= F; ++i)
    {
        auto cfg = simulation->newConfig();
        configList.emplace_back(cfg);

        keyList.emplace_back(cfg.NODE_SEED.getPublicKey());
        keyStrList.emplace_back(cfg.NODE_SEED.getStrKeyPublic());
    }

    // Peer D has an older overlay version so make sure we don't broadcast
    // messages to peers that don't support the survey messages
    configList[D].OVERLAY_PROTOCOL_VERSION =
        SurveyManager::MIN_OVERLAY_VERSION_FOR_SURVEY - 1;

    // B will only respond to/relay messages from A and E
    configList[B].SURVEYOR_KEYS.emplace(keyList[A]);
    configList[B].SURVEYOR_KEYS.emplace(keyList[E]);

    // Note that peer E is in SURVEYOR_KEYS of A and B, but is not in transitive
    // quorum, meaning that it's request messages will be dropped by relay nodes
    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(keyList[A]);
    qSet.validators.push_back(keyList[C]);

    for (int i = A; i <= F; ++i)
    {
        auto const& cfg = configList[i];
        simulation->addNode(cfg.NODE_SEED, qSet, &cfg);
    }

    // E->A->B->C->D B->F
    simulation->addConnection(keyList[E], keyList[A]);
    simulation->addConnection(keyList[A], keyList[B]);
    simulation->addConnection(keyList[B], keyList[C]);
    simulation->addConnection(keyList[B], keyList[F]);
    simulation->addConnection(keyList[C], keyList[D]);

    simulation->startAllNodes();

    // wait for ledgers to close so nodes get the updated transitive quorum
    int nLedgers = 1;
    simulation->crankUntil(
        [&simulation, nLedgers]() {
            return simulation->haveAllExternalized(nLedgers + 1, 1);
        },
        2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    REQUIRE(simulation->haveAllExternalized(nLedgers + 1, 1));

    auto getResults = [&](NodeID const& nodeID) {
        simulation->crankForAtLeast(std::chrono::seconds(1), false);
        auto strResult =
            simulation->getNode(nodeID)->getCommandHandler().manualCmd(
                "getsurveyresult");

        Json::Value result;
        Json::Reader reader;
        REQUIRE(reader.parse(strResult, result));
        return result;
    };

    auto sendRequest = [&](PublicKey const& surveyor,
                           PublicKey const& surveyed) {
        std::string topologyCmd = "surveytopology?duration=100&node=";
        simulation->getNode(surveyor)->getCommandHandler().manualCmd(
            topologyCmd + KeyUtils::toStrKey(surveyed));
    };

    auto crankForSurvey = [&]() {
        simulation->crankForAtLeast(
            configList[A].getExpectedLedgerCloseTime() *
                SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT,
            false);
    };

    SECTION("5 normal nodes (A->B->C->D B->F)")
    {
        sendRequest(keyList[A], keyList[B]);
        crankForSurvey();

        auto result = getResults(keyList[A]);
        Json::Value topology = result["topology"];

        REQUIRE(topology.size() == 1);

        // B responds with 2 new nodes (C and F)
        REQUIRE(topology[keyStrList[B]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[A]);

        std::set<std::string> expectedOutboundPeers = {keyStrList[F],
                                                       keyStrList[C]};
        std::set<std::string> actualOutboundPeers = {
            topology[keyStrList[B]]["outboundPeers"][0]["nodeId"].asString(),
            topology[keyStrList[B]]["outboundPeers"][1]["nodeId"].asString()};

        REQUIRE(expectedOutboundPeers == actualOutboundPeers);

        sendRequest(keyList[A], keyList[C]);
        sendRequest(keyList[A], keyList[F]);

        crankForSurvey();

        result = getResults(keyList[A]);
        topology = result["topology"];

        // In the next round, we sent requests to C and F
        REQUIRE(topology.size() == 3);
        REQUIRE(topology[keyStrList[C]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[B]);
        REQUIRE(topology[keyStrList[C]]["outboundPeers"][0]["nodeId"] ==
                keyStrList[D]);

        REQUIRE(topology[keyStrList[F]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[B]);
        REQUIRE(topology[keyStrList[F]]["outboundPeers"].isNull());

        sendRequest(keyList[A], keyList[D]);

        // move time forward. Nothing should happen because D has an older
        // overlay version
        crankForSurvey();

        // result stayed the same
        REQUIRE(topology.size() == 3);
    }

    SECTION("E is not in transitive quorum, so A doesn't respond or relay to B"
            "(E-/>A-/>B)")
    {
        sendRequest(keyList[E], keyList[A]);
        sendRequest(keyList[E], keyList[B]);

        // move time forward so next round of queries can go. requests should be
        // sent, but nodes shouldn't respond
        crankForSurvey();

        auto result = getResults(keyList[E]);
        Json::Value const& topology = result["topology"];

        REQUIRE(topology.size() == 2);
        for (auto const& value : topology)
        {
            REQUIRE(value.isNull());
        }
    }

    SECTION("B does not have C in SURVEYOR_KEYS, so B doesn't respond or relay "
            "to A (C-/>B-/>A)")
    {
        sendRequest(keyList[C], keyList[B]);
        sendRequest(keyList[C], keyList[A]);

        // move time forward so next round of queries can go
        crankForSurvey();

        auto result = getResults(keyList[C]);
        Json::Value const& topology = result["topology"];

        REQUIRE(topology.size() == 2);
        for (auto const& value : topology)
        {
            REQUIRE(value.isNull());
        }
    }

    simulation->stopAllNodes();
}
