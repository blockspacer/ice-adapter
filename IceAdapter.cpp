#include "IceAdapter.h"

#include <json/json.h>

#include "GPGNetServer.h"
#include "GPGNetMessage.h"
#include "JsonRpcServer.h"
#include "PeerRelay.h"
#include "IceAgent.h"
#include "logging.h"

namespace faf
{

IceAdapter::IceAdapter(IceAdapterOptions const& options,
                       Glib::RefPtr<Glib::MainLoop> mainloop):
  mOptions(options),
  mMainloop(mainloop)
{
  FAF_LOG_INFO << "ICE adapter version " << FAF_VERSION_STRING << " initializing";
  mRpcServer    = std::make_shared<JsonRpcServer>(mOptions.rpcPort);
  mGPGNetServer = std::make_shared<GPGNetServer>(mOptions.gpgNetPort);
  mGPGNetServer->addGpgMessageCallback(std::bind(&IceAdapter::onGpgNetMessage,
                                       this,
                                       std::placeholders::_1));
  mGPGNetServer->connectionChanged.connect(std::bind(&IceAdapter::onGpgConnectionStateChanged,
                                                     this,
                                                     std::placeholders::_1,
                                                     std::placeholders::_2));
  connectRpcMethods();

  auto resolver = Gio::Resolver::get_default();
  resolver->lookup_by_name_async(mOptions.stunHost, [this, resolver](Glib::RefPtr<Gio::AsyncResult>& result)
  {
    auto addresses = resolver->lookup_by_name_finish(result);
    if (addresses.size() == 0)
    {
      FAF_LOG_ERROR << "error looking up STUN hostname " << mOptions.stunHost;
    }
    else
    {
      mStunIp = (*addresses.begin())->to_string();
    }
  });
  resolver->lookup_by_name_async(mOptions.turnHost, [this, resolver](Glib::RefPtr<Gio::AsyncResult>& result)
  {
    auto addresses = resolver->lookup_by_name_finish(result);
    if (addresses.size() == 0)
    {
      FAF_LOG_ERROR << "error looking up TURN hostname " << mOptions.turnHost;
    }
    else
    {
      mTurnIp = (*addresses.begin())->to_string();
    }
  });
}

void IceAdapter::hostGame(std::string const& map)
{
  queueGameTask({IceAdapterGameTask::HostGame,
                 map,
                 "",
                 0});
}

void IceAdapter::joinGame(std::string const& remotePlayerLogin,
                          int remotePlayerId)
{
  createPeerRelay(remotePlayerId,
                  remotePlayerLogin,
                  false);
  queueGameTask({IceAdapterGameTask::JoinGame,
                 "",
                 remotePlayerLogin,
                 remotePlayerId});
}

void IceAdapter::connectToPeer(std::string const& remotePlayerLogin,
                               int remotePlayerId,
                               bool createOffer)
{
  createPeerRelay(remotePlayerId,
                  remotePlayerLogin,
                  createOffer);
  queueGameTask({IceAdapterGameTask::ConnectToPeer,
                 "",
                 remotePlayerLogin,
                 remotePlayerId});
}

void IceAdapter::reconnectToPeer(int remotePlayerId)
{
  auto relayIt = mRelays.find(remotePlayerId);
  if (relayIt == mRelays.end())
  {
    FAF_LOG_ERROR << "no relay for remote peer " << remotePlayerId << " found";
    return;
  }
  relayIt->second->reconnect();
}

void IceAdapter::disconnectFromPeer(int remotePlayerId)
{
  auto relayIt = mRelays.find(remotePlayerId);
  if (relayIt == mRelays.end())
  {
    FAF_LOG_ERROR << "no relay for remote peer " << remotePlayerId << " found";
    return;
  }
  mRelays.erase(relayIt);
  FAF_LOG_INFO << "removed relay for peer " << remotePlayerId;
  queueGameTask({IceAdapterGameTask::DisconnectFromPeer,
                 "",
                 "",
                 remotePlayerId});
}

void IceAdapter::addSdpMessage(int remotePlayerId, std::string const& type, std::string const& msg)
{
  auto relayIt = mRelays.find(remotePlayerId);
  if (relayIt == mRelays.end())
  {
    FAF_LOG_ERROR << "no relay for remote peer " << remotePlayerId << " found";
    return;
  }
  if(!relayIt->second->iceAgent())
  {
    FAF_LOG_ERROR << "!relayIt->second->iceAgent()";
    return;
  }
  if(relayIt->second->iceAgent()->peerConnectedToMe())
  {
    FAF_LOG_WARN << "relayIt->second->iceAgent()->isConnected()";
  }
  relayIt->second->iceAgent()->addRemoteSdpMessage(type, msg);
}

void IceAdapter::sendToGpgNet(GPGNetMessage const& message)
{
  if (mGPGNetServer->sessionCount() == 0)
  {
    FAF_LOG_ERROR << "sendToGpgNet failed. No sessions connected";
    return;
  }
  mGPGNetServer->sendMessage(message);
}

Json::Value IceAdapter::status() const
{
  Json::Value result;
  result["version"] = FAF_VERSION_STRING;
  /* options */
  {
    Json::Value options;

    options["player_id"]            = mOptions.localPlayerId;
    options["player_login"]         = std::string(mOptions.localPlayerLogin);
    options["rpc_port"]             = mOptions.rpcPort;
    options["ice_local_port_min"]   = mOptions.iceLocalPortMin;
    options["ice_local_port_max"]   = mOptions.iceLocalPortMax;
    options["use_upnp"]             = mOptions.useUpnp;
    options["gpgnet_port"]          = mOptions.gpgNetPort;
    options["lobby-port"]           = mOptions.gameUdpPort;
    options["stun_host"]            = std::string(mOptions.stunHost);
    options["turn_host"]            = std::string(mOptions.turnHost);
    options["turn_user"]            = std::string(mOptions.turnUser);
    options["turn_pass"]            = std::string(mOptions.turnPass);
    options["log_file"]             = std::string(mOptions.logFile);
    result["options"] = options;
  }
  /* GPGNet */
  {
    Json::Value gpgnet;

    gpgnet["local_port"] = mGPGNetServer->listenPort();
    gpgnet["connected"] = mGPGNetServer->sessionCount() > 0;
    gpgnet["game_state"] = mGPGNetGameState;

    /*
    if (!mHostGameMap.empty())
    {
      gpgnet["host_game"]["map"] = mHostGameMap;
    }
    else if(!mJoinGameRemotePlayerLogin.empty())
    {
      gpgnet["join_game"]["remote_player_login"] = mJoinGameRemotePlayerLogin;
      gpgnet["join_game"]["remote_player_id"] = mJoinGameRemotePlayerId;
    }
    */
    result["gpgnet"] = gpgnet;
  }
  /* Relays */
  {
    Json::Value relays(Json::arrayValue);
    for (auto it = mRelays.begin(), end = mRelays.end(); it != end; ++it)
    {
      Json::Value relay;
      relay["remote_player_id"] = it->first;
      relay["remote_player_login"] = it->second->peerLogin();
      relay["local_game_udp_port"] = it->second->localGameUdpPort();

      if (it->second->iceAgent())
      {
        relay["ice_agent"]["state"] = stateToString(it->second->iceAgent()->state());
        relay["ice_agent"]["peer_connected_to_me"] = it->second->iceAgent()->peerConnectedToMe();
        relay["ice_agent"]["connected_to_peer"] = it->second->iceAgent()->connectedToPeer();
        relay["ice_agent"]["local_candidate"] = it->second->iceAgent()->localCandidateInfo();
        relay["ice_agent"]["remote_candidate"] = it->second->iceAgent()->remoteCandidateInfo();
        relay["ice_agent"]["remote_sdp"] = it->second->iceAgent()->remoteSdp();
        relay["ice_agent"]["time_to_connected"] = it->second->iceAgent()->timeToConnected();
      }

      relays.append(relay);
    }
    result["relays"] = relays;
  }
  return result;
}

void IceAdapter::onGpgNetMessage(GPGNetMessage const& message)
{
  if (message.header == "GameState")
  {
    if (message.chunks.size() == 1)
    {
      mGPGNetGameState = message.chunks[0].asString();
      if (mGPGNetGameState == "Idle")
      {
        mGPGNetServer->sendCreateLobby(InitMode::NormalLobby,
                                       mOptions.gameUdpPort,
                                       mOptions.localPlayerLogin,
                                       mOptions.localPlayerId,
                                       1);
      }
      tryExecuteGameTasks();
    }
  }
  Json::Value rpcParams(Json::arrayValue);
  rpcParams.append(message.header);
  Json::Value msgChunks(Json::arrayValue);
  for(auto const& chunk : message.chunks)
  {
    msgChunks.append(chunk);
  }
  rpcParams.append(msgChunks);
  mRpcServer->sendRequest("onGpgNetMessageReceived",
                          rpcParams);
}

void IceAdapter::onGpgConnectionStateChanged(TcpSession* session, ConnectionState cs)
{
  if (mRpcServer->sessionCount() > 1)
  {
    FAF_LOG_ERROR << "only 1 game session supported!!";
  }
  Json::Value params(Json::arrayValue);
  params.append(cs == ConnectionState::Connected ? "Connected" : "Disconnected");
  mRpcServer->sendRequest("onConnectionStateChanged",
                          params);
  if (cs == ConnectionState::Disconnected)
  {
    FAF_LOG_INFO << "game disconnected";
    mRelays.clear();
    mGPGNetGameState = "";
  }
  else
  {
    FAF_LOG_INFO << "game connected";
  }
}

void IceAdapter::connectRpcMethods()
{
  if (!mRpcServer)
  {
    return;
  }

  mRpcServer->setRpcCallback("quit",
                             [this](Json::Value const& paramsArray,
                                    Json::Value & result,
                                    Json::Value & error,
                                    Socket* session)
  {
    result = "ok";
    mMainloop->quit();
  });

  mRpcServer->setRpcCallback("hostGame",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 1)
    {
      error = "Need 1 parameter: mapName (string)";
      return;
    }
    try
    {
      hostGame(paramsArray[0].asString());
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("joinGame",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 2)
    {
      error = "Need 2 parameters: remotePlayerLogin (string), remotePlayerId (int)";
      return;
    }
    try
    {
      joinGame(paramsArray[0].asString(), paramsArray[1].asInt());
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("connectToPeer",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 2)
    {
      error = "Need 2 parameters: remotePlayerLogin (string), remotePlayerId (int)";
      return;
    }
    try
    {
      connectToPeer(paramsArray[0].asString(),
                    paramsArray[1].asInt(),
                    paramsArray.size() > 2 ? paramsArray[2].asBool() : true);
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("reconnectToPeer",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 1)
    {
      error = "Need 1 parameters: remotePlayerId (int)";
      return;
    }
    try
    {
      reconnectToPeer(paramsArray[0].asInt());
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("disconnectFromPeer",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 1)
    {
      error = "Need 1 parameters: remotePlayerId (int)";
      return;
    }
    try
    {
      disconnectFromPeer(paramsArray[0].asInt());
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("addSdpMessage",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 3)
    {
      error = "Need 3 parameters: remotePlayerId (int), type (string), msg (string)";
      return;
    }
    try
    {
      addSdpMessage(paramsArray[0].asInt(),
                    paramsArray[1].asString(),
                    paramsArray[2].asString());
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("sendToGpgNet",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    if (paramsArray.size() < 2 ||
        !paramsArray[1].isArray())
    {
      error = "Need 2 parameters: header (string), chunks (array)";
      return;
    }
    try
    {
      GPGNetMessage message;
      message.header = paramsArray[0].asString();
      for(int i = 0; i < paramsArray[1].size(); ++i)
      {
        message.chunks.push_back(paramsArray[1][i]);
      }
      sendToGpgNet(message);
      result = "ok";
    }
    catch(std::exception& e)
    {
      error = e.what();
    }
  });

  mRpcServer->setRpcCallback("status",
                             [this](Json::Value const& paramsArray,
                             Json::Value & result,
                             Json::Value & error,
                             Socket* session)
  {
    result = status();
  });
}

void IceAdapter::queueGameTask(IceAdapterGameTask t)
{
  mGameTasks.push(t);
  tryExecuteGameTasks();
}

void IceAdapter::tryExecuteGameTasks()
{
  if (mGPGNetServer->sessionCount() == 0)
  {
    return;
  }
  while (!mGameTasks.empty())
  {
    auto task = mGameTasks.front();
    switch (task.task)
    {
      case IceAdapterGameTask::JoinGame:
      {
        if (mGPGNetGameState != "Lobby")
        {
          return;
        }
        auto relayIt = mRelays.find(task.remoteId);
        if (relayIt == mRelays.end())
        {
          FAF_LOG_ERROR << "no relay found for joining player " << task.remoteId;
          return;
        }
        else
        {
          mGPGNetServer->sendJoinGame(std::string("127.0.0.1:") + std::to_string(relayIt->second->localGameUdpPort()),
                                      task.remoteLogin,
                                      task.remoteId);
        }
        break;
      }
      case IceAdapterGameTask::HostGame:
        if (mGPGNetGameState != "Lobby")
        {
          return;
        }
        mGPGNetServer->sendHostGame(task.hostMap);
        break;
      case IceAdapterGameTask::ConnectToPeer:
      {
        auto relayIt = mRelays.find(task.remoteId);
        if (relayIt == mRelays.end())
        {
          FAF_LOG_ERROR << "no relay found for joining player " << task.remoteId;
        }
        else
        {
          mGPGNetServer->sendConnectToPeer(std::string("127.0.0.1:") + std::to_string(relayIt->second->localGameUdpPort()),
                                           task.remoteLogin,
                                           task.remoteId);
        }
        break;
      }
      case IceAdapterGameTask::DisconnectFromPeer:
        mGPGNetServer->sendDisconnectFromPeer(task.remoteId);
        break;
    }
    mGameTasks.pop();
  }
}

std::shared_ptr<PeerRelay> IceAdapter::createPeerRelay(int remotePlayerId,
                                                       std::string const& remotePlayerLogin,
                                                       bool createOffer)
{
  auto sdpMsgCb = [this](PeerRelay* relay, std::string const& type, std::string const& msg)
  {
    Json::Value gatheredSdpParams(Json::arrayValue);
    gatheredSdpParams.append(mOptions.localPlayerId);
    gatheredSdpParams.append(relay->peerId());
    gatheredSdpParams.append(type);
    gatheredSdpParams.append(msg);
    mRpcServer->sendRequest("onSdpMessage",
                            gatheredSdpParams);
  };

  auto stateCb = [this](PeerRelay* relay, IceAgentState const& state)
  {
    Json::Value iceStateParams(Json::arrayValue);
    iceStateParams.append(mOptions.localPlayerId);
    iceStateParams.append(relay->peerId());
    iceStateParams.append(stateToString(state));
    mRpcServer->sendRequest("onPeerStateChanged",
                            iceStateParams);
  };

  auto candSelectedCb = [this](PeerRelay* relay, std::string const& local, std::string const& remote)
  {
    Json::Value iceCandParams(Json::arrayValue);
    iceCandParams.append(mOptions.localPlayerId);
    iceCandParams.append(relay->peerId());
    iceCandParams.append(local);
    iceCandParams.append(remote);
    mRpcServer->sendRequest("onCandidateSelected",
                          iceCandParams);
  };

  auto result = std::make_shared<PeerRelay>(mMainloop,
                                            remotePlayerId,
                                            remotePlayerLogin,
                                            mStunIp,
                                            mTurnIp,
                                            sdpMsgCb,
                                            stateCb,
                                            candSelectedCb,
                                            createOffer,
                                            mOptions);
  mRelays[remotePlayerId] = result;

  if (createOffer)
  {
    result->iceAgent()->gatherCandidates();
  }

  result->iceAgent()->onPeerConnectedToMe.connect([this, remotePlayerId]()
  {
    Json::Value onConnectedToPeerParams(Json::arrayValue);
    onConnectedToPeerParams.append(mOptions.localPlayerId);
    onConnectedToPeerParams.append(remotePlayerId);
    mRpcServer->sendRequest("onIceConnected",
                            onConnectedToPeerParams);
  });

  return result;
}

}
