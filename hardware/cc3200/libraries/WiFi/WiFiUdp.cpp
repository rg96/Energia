/*
 WiFiUdp.cpp - Adaptation of Arduino WiFi library for Energia and CC3200 launchpad
 Author: Noah Luskey | LuskeyNoah@gmail.com
 
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.
 
 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

extern "C" {
    #include "utility/SimpleLink.h"
    #include "utility/socket.h"
}

#include <string.h>

#include "WiFi.h"
#include "WiFiUdp.h"
#include "WiFiClient.h"
#include "WiFiServer.h"

WiFiUDP::WiFiUDP() : _sock(NO_SOCKET_AVAIL)
{
    //
    //fill the buffers with zeroes
    //
    memset(rx_buf, 0, UDP_RX_PACKET_MAX_SIZE);
    memset(tx_buf, 0, UDP_TX_PACKET_MAX_SIZE);
    rx_currentIndex = 0;
    rx_fillLevel = 0;
    tx_fillLevel = 0;
    _remotePort = 0;
    _remoteIP = 0;
    
}

uint8_t WiFiUDP::begin(uint16_t port)
{
    
    //
    //get a socket from the WiFiClass (convoluted method from the arduino library)
    //
    int sock = WiFiClass::getSocket();
    if (sock == NO_SOCKET_AVAIL) {
        return 0;
    }
    
    //
    //get a socket handle from the simplelink api and make sure it's valid
    //
    int socketHandle = sl_Socket(SL_AF_INET, SL_SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle < 0) {
        return 0;
    }
    
    //
    //bind the socket to the requested port and check for success
    //if failure, gracefully close the socket and return
    //
    SlSockAddrIn_t portAddress;
    portAddress.sin_family = SL_AF_INET;
    portAddress.sin_port = sl_Htons(port);
    portAddress.sin_addr.s_addr = 0;
    int iRet = sl_Bind(socketHandle, (SlSockAddr_t*)&portAddress, sizeof(portAddress));
    if (iRet < 0) {
        sl_Close(socketHandle);
        return 0;
    }
    
    //
    //now that simplelink api calls are done, set the object's variables
    //
    _socketHandle = socketHandle;
    _port = port;
    _sock = sock;
    WiFiClass::_server_port[sock] = port;
    return 1;
}


int WiFiUDP::available()
{
    //
    //returns to number of bytes left to read in the current packet
    //bytesLeft should never be negative... but just in case it's restricted positive
    //
    int bytesLeft = rx_fillLevel - rx_currentIndex;
    if (bytesLeft < 0) {
        return 0;
    } else {
        return bytesLeft;
    }
}

void WiFiUDP::stop()
{
    //
    //close the socket and reset any important variables
    //
    flush();
    sl_Close(_socketHandle);
    WiFiClass::_server_port[_sock] = 0;
    _sock = NO_SOCKET_AVAIL;
}

int WiFiUDP::beginPacket(const char *host, uint16_t port)
{
    //
    //look up host's IP address
    //
    IPAddress ip;
    int success = WiFi.hostByName((char*)host, ip);
    
    //
    //if host successfully resolved to IP, begin packet to that IP
    //
    if (success) {
        return beginPacket(ip, port);
    } else {
        return 0;
    }
}

int WiFiUDP::beginPacket(IPAddress ip, uint16_t port)
{
    //
    //make sure a port has been created
    //!! this doesn't create a port if one doesn't exist. Is that ok?
    //
    if (_sock == NO_SOCKET_AVAIL) {
        return 0;
    }
    
    //
    //store the address information for when endPacket is called
    //
    _sendIP = ip;
    _sendPort = port;
    
    //
    //reset all tx buffer indicators
    //
    memset(tx_buf, 0, UDP_TX_PACKET_MAX_SIZE);
    rx_currentIndex = 0;
    tx_fillLevel = 0;
    
    return 1;
    
}

int WiFiUDP::endPacket()
{
    //
    //fill in the address structure
    //
    SlSockAddrIn_t sendAddress;
    sendAddress.sin_family = SL_AF_INET;
    sendAddress.sin_port = sl_Htons(_sendPort);
    sendAddress.sin_addr.s_addr = sl_Htonl(_sendIP);
    
    //
    //use the simplelink library to send the tx buffer
    //
    int iRet = sl_SendTo(_socketHandle, tx_buf, tx_fillLevel, NULL, (SlSockAddr_t*)&sendAddress, sizeof(SlSockAddrIn_t));
    if (iRet < 0) {
        return 0;
    }
    
    //
    //reset all tx buffer indicators
    //
    memset(tx_buf, 0, UDP_TX_PACKET_MAX_SIZE);
    tx_fillLevel = 0;
    return 1;
}

size_t WiFiUDP::write(uint8_t byte)
{
    //
    //write a single byte into the buffer using overloaded method
    //
    return write(&byte, 1);
}

size_t WiFiUDP::write(const uint8_t *buffer, size_t size)
{
    //
    //it's possible that size is more than can fit in the tx_buffer
    //so check it and make it smaller if necessary
    //
    if (tx_fillLevel + size > UDP_TX_PACKET_MAX_SIZE) {
        size = UDP_TX_PACKET_MAX_SIZE - tx_fillLevel;
    }
    
    
    //
    //copy the appropriate number of bytes into the buffer
    //
    memcpy(&tx_buf[tx_fillLevel], buffer, size);
    tx_fillLevel += size;
    return size;
}

//
//bit of a misnomer. This waits to receive a packet and then stores it in a buffer
//it is important that this is called before any of the read or available commands
//are called. This does the actual work. Read, peek, etc. are just organizational.
//
int WiFiUDP::parsePacket()
{
    //
    //make sure we actually have a socket
    //
    if (_socketHandle == NO_SOCKET_AVAIL) {
        return 0;
    }
    
    //
    //the sl_select command blocks until something interesting happens or
    //it times out (current timeout set for 10 ms, the minimum)
    //
    SlTimeval_t timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;
    
    SlFdSet_t readSocketHandles, errorSocketHandles;
    SL_FD_ZERO(&readSocketHandles);
    SL_FD_ZERO(&errorSocketHandles);
    SL_FD_SET(_socketHandle, &readSocketHandles);
    SL_FD_SET(_socketHandle, &errorSocketHandles);
    
    int iRet = sl_Select(_socketHandle+1, &readSocketHandles, NULL, &errorSocketHandles, &timeout);
    if (iRet <= 0) {
        return 0;
    }

    //
    //Since we've reached this point, the sl_select command has indicated
    //that either we're going to get an error, or an immediate read
    //
    SlSockAddrIn_t  address;
    int AddrSize = sizeof(address);
    int bytes = sl_RecvFrom(_socketHandle, rx_buf, UDP_RX_PACKET_MAX_SIZE, NULL, (SlSockAddr_t*)&address, (SlSocklen_t*)&AddrSize);

    //
    //store the sender's address (sl_HtonX reorders bits to processor order)
    //!! Although this follows some examples (upd_socket), it goes agains the
    //!! API documentation. The API maintains that the 5th arg to RecvFrom is not in/out
    //
    _remoteIP = sl_Htonl(address.sin_addr.s_addr);
    _remotePort = sl_Htons(address.sin_port);
    
    //
    //If an error occured, return 0, otherwise return the byte length of the packet
    //and reset the buffer index counter and fill level variables
    //
    if (bytes < 0) {
        rx_fillLevel = 0;
        return 0;
    } else {
        rx_currentIndex = 0;
        rx_fillLevel = bytes;
        return bytes;
    }
}

int WiFiUDP::read()
{
    //
    //don't read past the available data in the buffer
    //
    if (rx_currentIndex >= rx_fillLevel) {
        return 0;
    }
    
    //
    //return the byte at the current index and increment that index
    //
    return rx_buf[rx_currentIndex++];
}

int WiFiUDP::read(unsigned char* buffer, size_t len)
{
    //
    //copy the requested number of bytes up to the length of the packet
    //
    int bytesCopied = 0;
    while ((bytesCopied <= len) && (rx_currentIndex < rx_fillLevel)) {
        buffer[bytesCopied] = rx_buf[rx_currentIndex];
        bytesCopied++;
        rx_currentIndex++;
    }
    
    return bytesCopied;
}

int WiFiUDP::peek()
{
    //
    //return the next byte without incrementing the index counter
    //
    return rx_buf[rx_currentIndex];
}

void WiFiUDP::flush()
{
    //
    //destroy the remaining data in the buffer and reset index and length variables
    //
    memset(rx_buf, 0, UDP_RX_PACKET_MAX_SIZE);
    rx_currentIndex = 0;
    rx_fillLevel = 0;
}

IPAddress  WiFiUDP::remoteIP()
{
    //
    //this value is maintained by ParsePacket method
    //
    return _remoteIP;
}

uint16_t  WiFiUDP::remotePort()
{
    //
    //this value is maintained by ParsePacket method
    //
    return _remotePort;
}
