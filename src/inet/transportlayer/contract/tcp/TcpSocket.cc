//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//

#include "inet/common/packet/Message.h"
#include "inet/applications/common/SocketTag_m.h"
#include "inet/common/ProtocolTag_m.h"
#include "inet/transportlayer/contract/tcp/TcpSocket.h"

namespace inet {

TcpSocket::TcpSocket()
{
    // don't allow user-specified connIds because they may conflict with
    // automatically assigned ones.
    connId = getEnvir()->getUniqueNumber();
    sockstate = NOT_BOUND;

    localPrt = remotePrt = -1;
    cb = nullptr;
    yourPtr = nullptr;

    gateToTcp = nullptr;
}

TcpSocket::TcpSocket(cMessage *msg)
{
    connId = check_and_cast<Indication *>(msg)->getTag<SocketInd>()->getSocketId();
    sockstate = CONNECTED;

    localPrt = remotePrt = -1;
    cb = nullptr;
    yourPtr = nullptr;
    gateToTcp = nullptr;

    if (msg->getKind() == TCP_I_AVAILABLE) {
        TcpAvailableInfo *availableInfo = check_and_cast<TcpAvailableInfo *>(msg->getControlInfo());
        localAddr = availableInfo->getLocalAddr();
        remoteAddr = availableInfo->getRemoteAddr();
        localPrt = availableInfo->getLocalPort();
        remotePrt = availableInfo->getRemotePort();
    }
    else if (msg->getKind() == TCP_I_ESTABLISHED) {
        // management of stockstate is left to processMessage() so we always
        // set it to CONNECTED in the ctor, whatever TCP_I_xxx arrives.
        // However, for convenience we extract TcpConnectInfo already here, so that
        // remote address/port can be read already after the ctor call.

        TcpConnectInfo *connectInfo = check_and_cast<TcpConnectInfo *>(msg->getControlInfo());
        localAddr = connectInfo->getLocalAddr();
        remoteAddr = connectInfo->getRemoteAddr();
        localPrt = connectInfo->getLocalPort();
        remotePrt = connectInfo->getRemotePort();
    }
}

TcpSocket::~TcpSocket()
{
    if (cb)
        cb->socketDeleted(connId, yourPtr);
}

const char *TcpSocket::stateName(int state)
{
#define CASE(x)    case x: \
        s = #x; break
    const char *s = "unknown";
    switch (state) {
        CASE(NOT_BOUND);
        CASE(BOUND);
        CASE(LISTENING);
        CASE(CONNECTING);
        CASE(CONNECTED);
        CASE(PEER_CLOSED);
        CASE(LOCALLY_CLOSED);
        CASE(CLOSED);
        CASE(SOCKERROR);
    }
    return s;
#undef CASE
}

void TcpSocket::sendToTCP(cMessage *msg, int connId)
{
    if (!gateToTcp)
        throw cRuntimeError("TcpSocket: setOutputGate() must be invoked before socket can be used");

    auto& tags = getTags(msg);
    tags.addTagIfAbsent<DispatchProtocolReq>()->setProtocol(&Protocol::tcp);
    tags.addTagIfAbsent<SocketReq>()->setSocketId(connId == -1 ? this->connId : connId);
    check_and_cast<cSimpleModule *>(gateToTcp->getOwnerModule())->send(msg, gateToTcp);
}

void TcpSocket::bind(int lPort)
{
    if (sockstate != NOT_BOUND)
        throw cRuntimeError("TcpSocket::bind(): socket already bound");

    if (lPort < 0 || lPort > 65535)
        throw cRuntimeError("TcpSocket::bind(): invalid port number %d", lPort);

    localPrt = lPort;
    sockstate = BOUND;
}

void TcpSocket::bind(L3Address lAddr, int lPort)
{
    if (sockstate != NOT_BOUND)
        throw cRuntimeError("TcpSocket::bind(): socket already bound");

    // allow -1 here, to make it possible to specify address only
    if ((lPort < 0 || lPort > 65535) && lPort != -1)
        throw cRuntimeError("TcpSocket::bind(): invalid port number %d", lPort);

    localAddr = lAddr;
    localPrt = lPort;
    sockstate = BOUND;
}

void TcpSocket::listen(bool fork)
{
    if (sockstate != BOUND)
        throw cRuntimeError(sockstate == NOT_BOUND ? "TcpSocket: must call bind() before listen()"
                : "TcpSocket::listen(): connect() or listen() already called");

    auto request = new Request("PassiveOPEN", TCP_C_OPEN_PASSIVE);

    TcpOpenCommand *openCmd = new TcpOpenCommand();
    openCmd->setLocalAddr(localAddr);
    openCmd->setLocalPort(localPrt);
    openCmd->setFork(fork);
    openCmd->setTcpAlgorithmClass(tcpAlgorithmClass.c_str());

    request->setControlInfo(openCmd);
    sendToTCP(request);
    sockstate = LISTENING;
}

void TcpSocket::accept(int socketId)
{
    auto request = new Request("ACCEPT", TCP_C_ACCEPT);
    TcpAcceptCommand *acceptCmd = new TcpAcceptCommand();
    request->setControlInfo(acceptCmd);
    sendToTCP(request, socketId);
}

void TcpSocket::connect(L3Address remoteAddress, int remotePort)
{
    if (sockstate != NOT_BOUND && sockstate != BOUND)
        throw cRuntimeError("TcpSocket::connect(): connect() or listen() already called (need renewSocket()?)");

    if (remotePort < 0 || remotePort > 65535)
        throw cRuntimeError("TcpSocket::connect(): invalid remote port number %d", remotePort);

    auto request = new Request("ActiveOPEN", TCP_C_OPEN_ACTIVE);

    remoteAddr = remoteAddress;
    remotePrt = remotePort;

    TcpOpenCommand *openCmd = new TcpOpenCommand();
    openCmd->setLocalAddr(localAddr);
    openCmd->setLocalPort(localPrt);
    openCmd->setRemoteAddr(remoteAddr);
    openCmd->setRemotePort(remotePrt);
    openCmd->setTcpAlgorithmClass(tcpAlgorithmClass.c_str());

    request->setControlInfo(openCmd);
    sendToTCP(request);
    sockstate = CONNECTING;
}

void TcpSocket::send(Packet *msg)
{
    if (sockstate != CONNECTED && sockstate != CONNECTING && sockstate != PEER_CLOSED)
        throw cRuntimeError("TcpSocket::send(): socket not connected or connecting, state is %s", stateName(sockstate));

    msg->setKind(TCP_C_SEND);
    sendToTCP(msg);
}

void TcpSocket::sendCommand(Request *msg)
{
    sendToTCP(msg);
}

void TcpSocket::close()
{
    if (sockstate != CONNECTED && sockstate != PEER_CLOSED && sockstate != CONNECTING && sockstate != LISTENING)
        throw cRuntimeError("TcpSocket::close(): not connected or close() already called (sockstate=%s)", stateName(sockstate));

    auto request = new Request("CLOSE", TCP_C_CLOSE);
    TcpCommand *cmd = new TcpCommand();
    request->setControlInfo(cmd);
    sendToTCP(request);
    sockstate = (sockstate == CONNECTED) ? LOCALLY_CLOSED : CLOSED;
}

void TcpSocket::abort()
{
    if (sockstate != NOT_BOUND && sockstate != BOUND && sockstate != CLOSED && sockstate != SOCKERROR) {
        auto request = new Request("ABORT", TCP_C_ABORT);
        TcpCommand *cmd = new TcpCommand();
        request->setControlInfo(cmd);
        sendToTCP(request);
    }
    sockstate = CLOSED;
}

void TcpSocket::requestStatus()
{
    auto request = new Request("STATUS", TCP_C_STATUS);
    TcpCommand *cmd = new TcpCommand();
    request->setControlInfo(cmd);
    sendToTCP(request);
}

void TcpSocket::renewSocket()
{
    connId = getEnvir()->getUniqueNumber();
    remoteAddr = localAddr = L3Address();
    remotePrt = localPrt = -1;
    sockstate = NOT_BOUND;
}

bool TcpSocket::belongsToSocket(cMessage *msg)
{
    auto& tags = getTags(msg);
    return tags.getTag<SocketInd>()->getSocketId() == connId;
}

void TcpSocket::setCallbackObject(ICallback *callback, void *yourPointer)
{
    cb = callback;
    yourPtr = yourPointer;
}

void TcpSocket::processMessage(cMessage *msg)
{
    ASSERT(belongsToSocket(msg));

    TcpStatusInfo *status;
    TcpAvailableInfo *availableInfo;
    TcpConnectInfo *connectInfo;

    switch (msg->getKind()) {
        case TCP_I_DATA:
            if (cb)
                cb->socketDataArrived(connId, yourPtr, check_and_cast<Packet*>(msg), false);
            else
                delete msg;

            break;

        case TCP_I_URGENT_DATA:
            if (cb)
                cb->socketDataArrived(connId, yourPtr, check_and_cast<Packet*>(msg), true);
            else
                delete msg;

            break;

        case TCP_I_AVAILABLE:
            availableInfo = check_and_cast<TcpAvailableInfo *>(msg->getControlInfo());
            // TODO: implement non-auto accept support, by accepting using the callback interface
            accept(availableInfo->getNewSocketId());

            if (cb)
                cb->socketAvailable(connId, yourPtr, availableInfo);
            delete msg;

            break;

        case TCP_I_ESTABLISHED:
            // Note: this code is only for sockets doing active open, and nonforking
            // listening sockets. For a forking listening sockets, TCP_I_ESTABLISHED
            // carries a new connId which won't match the connId of this TcpSocket,
            // so you won't get here. Rather, when you see TCP_I_ESTABLISHED, you'll
            // want to create a new TcpSocket object via new TcpSocket(msg).
            sockstate = CONNECTED;
            connectInfo = check_and_cast<TcpConnectInfo *>(msg->getControlInfo());
            localAddr = connectInfo->getLocalAddr();
            remoteAddr = connectInfo->getRemoteAddr();
            localPrt = connectInfo->getLocalPort();
            remotePrt = connectInfo->getRemotePort();
            delete msg;

            if (cb)
                cb->socketEstablished(connId, yourPtr);

            break;

        case TCP_I_PEER_CLOSED:
            sockstate = PEER_CLOSED;
            delete msg;

            if (cb)
                cb->socketPeerClosed(connId, yourPtr);

            break;

        case TCP_I_CLOSED:
            sockstate = CLOSED;
            delete msg;

            if (cb)
                cb->socketClosed(connId, yourPtr);

            break;

        case TCP_I_CONNECTION_REFUSED:
        case TCP_I_CONNECTION_RESET:
        case TCP_I_TIMED_OUT:
            sockstate = SOCKERROR;

            if (cb)
                cb->socketFailure(connId, yourPtr, msg->getKind());

            delete msg;
            break;

        case TCP_I_STATUS:
            status = check_and_cast<TcpStatusInfo *>(msg->removeControlInfo());
            delete msg;

            if (cb)
                cb->socketStatusArrived(connId, yourPtr, status);

            break;

        default:
            throw cRuntimeError("TcpSocket: invalid msg kind %d, one of the TCP_I_xxx constants expected",
                msg->getKind());
    }
}

} // namespace inet

