/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Nathan Osman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <QtGlobal>

#ifdef Q_OS_LINUX
#  include <cerrno>
#  include <cstring>
#  include <sys/socket.h>
#endif

#include <QHostInfo>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>

#include "mdns.h"
#include "mdnsquery.h"
#include "mdnsserver.h"

const quint32 DefaultTtl = 60 * 60;

// TODO: watch for the sockets disconnecting and retry hostname registration

MdnsServer::MdnsServer()
    : mHostnameConfirmed(false)
{
    connect(&mSocketTimer, &QTimer::timeout, this, &MdnsServer::onSocketTimeout);
    connect(&mHostnameTimer, &QTimer::timeout, this, &MdnsServer::onHostnameTimeout);
    connect(&mIpv4Socket, &QUdpSocket::readyRead, this, &MdnsServer::onReadyRead);
    connect(&mIpv6Socket, &QUdpSocket::readyRead, this, &MdnsServer::onReadyRead);
    connect(this, &MdnsServer::messageReceived, this, &MdnsServer::onMessageReceived);

    // Prepare the timers
    mSocketTimer.setSingleShot(true);
    mHostnameTimer.setSingleShot(true);

    // Start joining the multicast addresses
    onSocketTimeout();
}

QString MdnsServer::hostname() const
{
    return mHostname;
}

void MdnsServer::sendMessage(const MdnsMessage &message)
{
    QByteArray packet;
    Mdns::toPacket(message, packet);
    if (message.protocol() == Mdns::Protocol::IPv4) {
        mIpv4Socket.writeDatagram(packet, message.address(), message.port());
    }
    if (message.protocol() == Mdns::Protocol::IPv6) {
        mIpv6Socket.writeDatagram(packet, message.address(), message.port());
    }
}

bool MdnsServer::generateRecord(const QHostAddress &address, quint16 type, MdnsRecord &record)
{
    // Find the interface that the address belongs to
    foreach (QNetworkInterface interface, QNetworkInterface::allInterfaces()) {
        foreach (QNetworkAddressEntry entry, interface.addressEntries()) {
            if (address.isInSubnet(entry.ip(), entry.prefixLength())) {

                // Loop through all of the interface addresses and find one
                // that matches the requested type
                foreach (QHostAddress address, interface.allAddresses()) {
                    if ((address.protocol() == QAbstractSocket::IPv4Protocol && type == Mdns::A) ||
                            (address.protocol() == QAbstractSocket::IPv6Protocol && type == Mdns::AAAA)) {
                        record.setName(mHostname.toUtf8());
                        record.setType(type);
                        record.setTtl(DefaultTtl);
                        record.setAddress(entry.ip());
                        return true;
                    }
                }
                break;
            }
        }
    }
    return false;
}

void MdnsServer::onSocketTimeout()
{
    // Bind the sockets if not already bound
    if (mIpv4Socket.state() != QAbstractSocket::BoundState) {
        bindSocket(mIpv4Socket, QHostAddress::AnyIPv4);
    }
    if (mIpv6Socket.state() != QAbstractSocket::BoundState) {
        bindSocket(mIpv6Socket, QHostAddress::AnyIPv6);
    }
    bool ipv4Bound = mIpv4Socket.state() == QAbstractSocket::BoundState;
    bool ipv6Bound = mIpv6Socket.state() == QAbstractSocket::BoundState;

    // Assuming either of the sockets are bound, join multicast groups
    if (ipv4Bound || ipv6Bound) {
        foreach (QNetworkInterface interface, QNetworkInterface::allInterfaces()) {
            if (interface.flags() & QNetworkInterface::CanMulticast) {
                bool ipv4Address = false;
                bool ipv6Address = false;
                foreach (QHostAddress address, interface.allAddresses()) {
                    ipv4Address = ipv4Address || address.protocol() == QAbstractSocket::IPv4Protocol;
                    ipv6Address = ipv6Address || address.protocol() == QAbstractSocket::IPv6Protocol;
                }
                if (ipv4Bound && ipv6Address) {
                    mIpv4Socket.joinMulticastGroup(Mdns::Ipv4Address, interface);
                }
                if (ipv6Bound && ipv6Address) {
                    mIpv6Socket.joinMulticastGroup(Mdns::Ipv6Address, interface);
                }
            }
        }

        // If the hostname has not been set, begin checking hostnames
        if (!mHostnameConfirmed) {
            mHostname = QHostInfo::localHostName() + ".local.";
            mHostnameSuffix = 2;
            checkHostname(Mdns::Protocol::IPv4);
            checkHostname(Mdns::Protocol::IPv6);
            mHostnameTimer.start(2 * 1000);
        }
    }

    // Run the method again in one minute
    mSocketTimer.start(60 * 1000);
}

void MdnsServer::onHostnameTimeout()
{
    // There was no response for the hostname query, so it can be used
    mHostnameConfirmed = true;
}

void MdnsServer::onReadyRead()
{
    // Read the packet from the socket
    QUdpSocket *socket = dynamic_cast<QUdpSocket*>(sender());
    QByteArray packet;
    packet.resize(socket->pendingDatagramSize());
    QHostAddress address;
    quint16 port;
    socket->readDatagram(packet.data(), packet.size(), &address, &port);

    // Attempt to decode the packet
    MdnsMessage message;
    if (Mdns::fromPacket(packet, message)) {
        message.setAddress(address);
        message.setProtocol(address.protocol() == QAbstractSocket::IPv4Protocol ?
            Mdns::Protocol::IPv4 : Mdns::Protocol::IPv6);
        message.setPort(port);

        if (mHostnameConfirmed) {
            emit messageReceived(message);
        } else {

            // The only message we are interested in is one that indicates our
            // chosen hostname is already in use
            if (message.isResponse()) {
                foreach (MdnsRecord record, message.records()) {
                    if ((record.type() == Mdns::A || record.type() == Mdns::AAAA) &&
                            record.name() == mHostname && record.ttl()) {
                        QString suffix = QString("-%1").arg(mHostnameSuffix++);
                        mHostname = QString("%1%2.local.").arg(QHostInfo::localHostName()).arg(suffix);
                        checkHostname(Mdns::Protocol::IPv4);
                        checkHostname(Mdns::Protocol::IPv6);
                        break;
                    }
                }
            }
        }
    }
}

void MdnsServer::onMessageReceived(const MdnsMessage &message)
{
    if (!message.isResponse()) {

        // Check to see if any of the queries were for this device
        bool queryA = false;
        bool queryAAAA = false;
        foreach (MdnsQuery query, message.queries()) {
            if (query.name() == mHostname) {
                queryA = queryA || query.type() == Mdns::A;
                queryAAAA = queryAAAA || query.type() == Mdns::AAAA;
            }
        }

        // If there was a query for either the A or AAAA record, then
        // attempt to respond with the desired records, using the source
        // message address to determine which address to use
        if (queryA || queryAAAA) {
            MdnsMessage reply = message.reply();
            MdnsRecord ipv4Record;
            MdnsRecord ipv6Record;
            if (queryA && generateRecord(message.address(), Mdns::A, ipv4Record)) {
                reply.addRecord(ipv4Record);
            }
            if (queryAAAA && generateRecord(message.address(), Mdns::AAAA, ipv6Record)) {
                reply.addRecord(ipv6Record);
            }
            if (reply.records().length()) {
                sendMessage(reply);
            }
        }
    }
}

void MdnsServer::bindSocket(QUdpSocket &socket, const QHostAddress &address)
{
    // I cannot find the correct combination of flags that allows the socket
    // to bind properly on Linux, so on that platform, we must manually create
    // the socket and initialize the QUdpSocket with it

#ifdef Q_OS_UNIX
    if (!socket.bind(address, Mdns::Port, QAbstractSocket::ShareAddress)) {
        int arg = 1;
        if (setsockopt(socket.socketDescriptor(), SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<char*>(&arg), sizeof(int))) {
            emit error(strerror(errno));
        }
#endif
        if (!socket.bind(address, Mdns::Port, QAbstractSocket::ReuseAddressHint)) {
            emit error(socket.errorString());
        }
#ifdef Q_OS_UNIX
    }
#endif
}

void MdnsServer::checkHostname(Mdns::Protocol protocol)
{
    MdnsQuery query;
    query.setName(mHostname.toUtf8());
    query.setType(protocol == Mdns::Protocol::IPv4 ? Mdns::A : Mdns::AAAA);

    MdnsMessage message;
    message.setAddress(protocol == Mdns::Protocol::IPv4 ?
        Mdns::Ipv4Address : Mdns::Ipv6Address);
    message.setProtocol(protocol);
    message.setPort(Mdns::Port);
    message.addQuery(query);

    sendMessage(message);
}
