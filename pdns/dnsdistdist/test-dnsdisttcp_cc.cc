/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_NO_MAIN

#include <boost/test/unit_test.hpp>

#include "dnswriter.hh"
#include "dnsdist.hh"
#include "dnsdist-proxy-protocol.hh"
#include "dnsdist-rings.hh"
#include "dnsdist-tcp-downstream.hh"
#include "dnsdist-tcp-upstream.hh"

struct DNSDistStats g_stats;
GlobalStateHolder<NetmaskGroup> g_ACL;
GlobalStateHolder<vector<DNSDistRuleAction> > g_rulactions;
GlobalStateHolder<vector<DNSDistResponseRuleAction> > g_resprulactions;
GlobalStateHolder<vector<DNSDistResponseRuleAction> > g_cachehitresprulactions;
GlobalStateHolder<vector<DNSDistResponseRuleAction> > g_selfansweredresprulactions;
GlobalStateHolder<servers_t> g_dstates;

QueryCount g_qcount;

bool checkDNSCryptQuery(const ClientState& cs, PacketBuffer& query, std::shared_ptr<DNSCryptQuery>& dnsCryptQuery, time_t now, bool tcp)
{
  return false;
}

bool checkQueryHeaders(const struct dnsheader* dh)
{
  return true;
}

uint64_t uptimeOfProcess(const std::string& str)
{
  return 0;
}

uint64_t getLatencyCount(const std::string&)
{
  return 0;
}

static std::function<ProcessQueryResult(DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend)> s_processQuery;

ProcessQueryResult processQuery(DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend)
{
  if (s_processQuery) {
    return s_processQuery(dq, cs, holders, selectedBackend);
  }

  return ProcessQueryResult::Drop;
}

static std::function<bool(const PacketBuffer& response, const DNSName& qname, const uint16_t qtype, const uint16_t qclass, const ComboAddress& remote, unsigned int& qnameWireLength)> s_responseContentMatches;

bool responseContentMatches(const PacketBuffer& response, const DNSName& qname, const uint16_t qtype, const uint16_t qclass, const ComboAddress& remote, unsigned int& qnameWireLength)
{
  if (s_responseContentMatches) {
    return s_responseContentMatches(response, qname, qtype, qclass, remote, qnameWireLength);
  }

  return true;
}

static std::function<bool(PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted)> s_processResponse;

bool processResponse(PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted)
{
  if (s_processResponse) {
    return s_processResponse(response, localRespRulactions, dr, muted);
  }

  return false;
}

BOOST_AUTO_TEST_SUITE(test_dnsdisttcp_cc)

struct ExpectedStep
{
public:
  enum class ExpectedRequest { handshakeClient, readFromClient, writeToClient, closeClient, connectToBackend, readFromBackend, writeToBackend, closeBackend };

  ExpectedStep(ExpectedRequest r, IOState n, size_t b = 0, std::function<void(int descriptor, const ExpectedStep& step)> fn = nullptr): cb(fn), request(r), nextState(n), bytes(b)
  {
  }

  std::function<void(int descriptor, const ExpectedStep& step)> cb{nullptr};
  ExpectedRequest request;
  IOState nextState;
  size_t bytes{0};
};

static std::deque<ExpectedStep> s_steps;

static PacketBuffer s_readBuffer;
static PacketBuffer s_writeBuffer;
static PacketBuffer s_backendReadBuffer;
static PacketBuffer s_backendWriteBuffer;

std::ostream& operator<<(std::ostream &os, const ExpectedStep::ExpectedRequest d);

std::ostream& operator<<(std::ostream &os, const ExpectedStep::ExpectedRequest d)
{
  static const std::vector<std::string> requests = { "handshake with client", "read from client", "write to client", "close connection to client", "connect to the backend", "read from the backend", "write to the backend", "close connection to backend" };
  os<<requests.at(static_cast<size_t>(d));
  return os;
}

class MockupTLSConnection : public TLSConnection
{
public:
  MockupTLSConnection(int descriptor, bool client = false): d_descriptor(descriptor), d_client(client)
  {
  }

  ~MockupTLSConnection() { }

  IOState tryHandshake() override
  {
    auto step = getStep();
    BOOST_REQUIRE_EQUAL(step.request, ExpectedStep::ExpectedRequest::handshakeClient);

    return step.nextState;
  }

  IOState tryWrite(const PacketBuffer& buffer, size_t& pos, size_t toWrite) override
  {
    if (buffer.size() < toWrite || pos >= toWrite) {
      throw std::out_of_range("Calling tryWrite() with a too small buffer (" + std::to_string(buffer.size()) + ") for a write of " + std::to_string(toWrite - pos) + " bytes starting at " + std::to_string(pos));
    }

    auto step = getStep();
    BOOST_REQUIRE_EQUAL(step.request, !d_client ? ExpectedStep::ExpectedRequest::writeToClient : ExpectedStep::ExpectedRequest::writeToBackend);

    if (step.bytes == 0) {
      if (step.nextState == IOState::NeedWrite) {
        return step.nextState;
      }
      throw std::runtime_error("Remote host closed the connection");
    }

    toWrite -= pos;
    BOOST_REQUIRE_GE(buffer.size(), pos + toWrite);

    if (step.bytes < toWrite) {
      toWrite = step.bytes;
    }

    auto& externalBuffer = d_client ? s_backendWriteBuffer : s_writeBuffer;
    externalBuffer.insert(externalBuffer.end(), buffer.begin() + pos, buffer.begin() + pos + toWrite);
    pos += toWrite;

    return step.nextState;
  }

  IOState tryRead(PacketBuffer& buffer, size_t& pos, size_t toRead) override
  {
    if (buffer.size() < toRead || pos >= toRead) {
      throw std::out_of_range("Calling tryRead() with a too small buffer (" + std::to_string(buffer.size()) + ") for a read of " + std::to_string(toRead - pos) + " bytes starting at " + std::to_string(pos));
    }

    auto step = getStep();
    BOOST_REQUIRE_EQUAL(step.request, !d_client ? ExpectedStep::ExpectedRequest::readFromClient : ExpectedStep::ExpectedRequest::readFromBackend);

    if (step.bytes == 0) {
      if (step.nextState == IOState::NeedRead) {
        return step.nextState;
      }
      throw std::runtime_error("Remote host closed the connection");
    }

    auto& externalBuffer = d_client ? s_backendReadBuffer : s_readBuffer;
    toRead -= pos;

    if (step.bytes < toRead) {
      toRead = step.bytes;
    }

    BOOST_REQUIRE_GE(buffer.size(), toRead);
    BOOST_REQUIRE_GE(externalBuffer.size(), toRead);

    std::copy(externalBuffer.begin(), externalBuffer.begin() + toRead, buffer.begin() + pos);
    pos += toRead;
    externalBuffer.erase(externalBuffer.begin(), externalBuffer.begin() + toRead);

    return step.nextState;
  }

  IOState tryConnect(bool fastOpen, const ComboAddress& remote) override
  {
    auto step = getStep();
    BOOST_REQUIRE_EQUAL(step.request, ExpectedStep::ExpectedRequest::connectToBackend);

    return step.nextState;
  }

  void close() override
  {
    auto step = getStep();
    BOOST_REQUIRE_EQUAL(step.request, !d_client ? ExpectedStep::ExpectedRequest::closeClient : ExpectedStep::ExpectedRequest::closeBackend);
  }

  bool hasBufferedData() const override
  {
    return false;
  }

  std::string getServerNameIndication() const override
  {
    return "";
  }

  LibsslTLSVersion getTLSVersion() const override
  {
    return LibsslTLSVersion::TLS13;
  }

  bool hasSessionBeenResumed() const override
  {
    return false;
  }

  /* unused in that context, don't bother */
  void doHandshake() override
  {
  }

  void connect(bool fastOpen, const ComboAddress& remote, unsigned int timeout) override
  {
  }

  size_t read(void* buffer, size_t bufferSize, unsigned int readTimeout, unsigned int totalTimeout=0) override
  {
    return 0;
  }

  size_t write(const void* buffer, size_t bufferSize, unsigned int writeTimeout) override
  {
    return 0;
  }
private:
  ExpectedStep getStep() const
  {
    BOOST_REQUIRE(!s_steps.empty());
    auto step = s_steps.front();
    s_steps.pop_front();

    if (step.cb) {
      step.cb(d_descriptor, step);
    }

    return step;
  }

  const int d_descriptor;
  bool d_client{false};
};

class MockupTLSCtx : public TLSCtx
{
public:
  ~MockupTLSCtx()
  {
  }

  std::unique_ptr<TLSConnection> getConnection(int socket, unsigned int timeout, time_t now) override
  {
    return std::make_unique<MockupTLSConnection>(socket);
  }

  std::unique_ptr<TLSConnection> getClientConnection(const std::string& host, int socket, unsigned int timeout) override
  {
    return std::make_unique<MockupTLSConnection>(socket, true);
  }

  void rotateTicketsKey(time_t now) override
  {
  }

  size_t getTicketsKeysCount() override
  {
    return 0;
  }
};

class MockupFDMultiplexer : public FDMultiplexer
{
public:
  MockupFDMultiplexer()
  {
  }

  ~MockupFDMultiplexer()
  {
  }

  int run(struct timeval* tv, int timeout=500) override
  {
    int ret = 0;

    gettimeofday(tv, nullptr); // MANDATORY

    /* 'ready' might be altered by a callback while we are iterating */
    const auto readyFDs = ready;
    for (const auto fd : readyFDs) {
      {
        const auto& it = d_readCallbacks.find(fd);

        if (it != d_readCallbacks.end()) {
          it->d_callback(it->d_fd, it->d_parameter);
          continue; // so we don't refind ourselves as writable!
        }
      }

      {
        const auto& it = d_writeCallbacks.find(fd);

        if (it != d_writeCallbacks.end()) {
          it->d_callback(it->d_fd, it->d_parameter);
        }
      }
    }

    return ret;
  }

  void getAvailableFDs(std::vector<int>& fds, int timeout) override
  {
  }

  void addFD(callbackmap_t& cbmap, int fd, callbackfunc_t toDo, const funcparam_t& parameter, const struct timeval* ttd=nullptr) override
  {
    accountingAddFD(cbmap, fd, toDo, parameter, ttd);
  }

  void removeFD(callbackmap_t& cbmap, int fd) override
  {
    accountingRemoveFD(cbmap, fd);
  }

  void alterFD(callbackmap_t& from, callbackmap_t& to, int fd, callbackfunc_t toDo, const funcparam_t& parameter, const struct timeval* ttd) override
  {
    accountingRemoveFD(from, fd);
    accountingAddFD(to, fd, toDo, parameter, ttd);
  }

  string getName() const override
  {
    return "mockup";
  }

  void setReady(int fd)
  {
    ready.insert(fd);
  }

  void setNotReady(int fd)
  {
    ready.erase(fd);
  }

private:
  std::set<int> ready;
};

BOOST_AUTO_TEST_CASE(test_IncomingConnection_SelfAnswered)
{
  ComboAddress local("192.0.2.1:80");
  ClientState localCS(local, true, false, false, "", {});
  auto tlsCtx = std::make_shared<MockupTLSCtx>();
  localCS.tlsFrontend = std::make_shared<TLSFrontend>(tlsCtx);

  TCPClientThreadData threadData;
  threadData.mplexer = std::make_unique<MockupFDMultiplexer>();

  struct timeval now;
  gettimeofday(&now, nullptr);

  PacketBuffer query;
  GenericDNSPacketWriter<PacketBuffer> pwQ(query, DNSName("powerdns.com."), QType::A, QClass::IN, 0);
  pwQ.getHeader()->rd = 1;

  uint16_t querySize = static_cast<uint16_t>(query.size());
  const uint8_t sizeBytes[] = { static_cast<uint8_t>(querySize / 256), static_cast<uint8_t>(querySize % 256) };
  query.insert(query.begin(), sizeBytes, sizeBytes + 2);

  g_proxyProtocolACL.clear();

  {
    /* drop right away */
    cerr<<"=> drop right away"<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      return ProcessQueryResult::Drop;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
  }

  {
    /* self-generated REFUSED, client closes connection right away */
    cerr<<"=> self-gen"<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, 65537 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      // Would be nicer to actually turn it into a response
      return ProcessQueryResult::SendAnswer;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size());
    BOOST_CHECK(s_writeBuffer == query);
  }

  {
    cerr<<"=> shorts"<<endl;
    /* need write then read during handshake,
       short read on the size, then on the query itself,
       self-generated REFUSED, short write on the response,
       client closes connection right away */
    s_readBuffer = query;
    s_writeBuffer.clear();
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::NeedWrite },
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::NeedRead },
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::NeedRead, 1 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 1 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::NeedRead, query.size() - 3 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 1 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::NeedWrite, query.size() - 1},
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, 1 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      // Would be nicer to actually turn it into a response
      return ProcessQueryResult::SendAnswer;
    };

    /* mark the incoming FD as always ready */
    dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setReady(-1);

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    while (threadData.mplexer->getWatchedFDCount(false) != 0 || threadData.mplexer->getWatchedFDCount(true) != 0) {
      threadData.mplexer->run(&now);
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size());
    BOOST_CHECK(s_writeBuffer == query);
  }

  {
    cerr<<"=> exception while handling the query"<<endl;
    /* Exception raised while handling the query */
    s_readBuffer = query;
    s_writeBuffer.clear();
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      throw std::runtime_error("Something unexpected happened");
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
  }

  {
#if 0
    cerr<<"=> 10k self-generated pipelined on the same connection"<<endl;

    /* 10k self-generated REFUSED pipelined on the same connection */
    size_t count = 10000;
    s_readBuffer.clear();
    s_writeBuffer.clear();
    s_steps = { { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done } };

    for (size_t idx = 0; idx < count; idx++) {
      s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
      s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 });
      s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 });
      s_steps.push_back({ ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() + 2 });
    };
    s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 });
    s_steps.push_back({ ExpectedStep::ExpectedRequest::closeClient, IOState::Done });

    size_t counter = 0;
    s_processQuery = [&counter](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      // Would be nicer to actually turn it into a response
      return ProcessQueryResult::SendAnswer;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size() * count);
#endif
  }

  {
    cerr<<"=> timeout while reading the query"<<endl;
    /* timeout while reading the query */
    s_readBuffer = query;
    s_writeBuffer.clear();
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::NeedRead, query.size() - 2 - 2 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      /* should not be reached */
      BOOST_CHECK(false);
      return ProcessQueryResult::SendAnswer;
    };

    /* mark the incoming FD as NOT ready */
    dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setNotReady(-1);

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(threadData.mplexer->run(&now), 0U);
    struct timeval later = now;
    later.tv_sec += g_tcpRecvTimeout + 1;
    auto expiredReadConns = threadData.mplexer->getTimeouts(later, false);
    for (const auto& cbData : expiredReadConns) {
      BOOST_CHECK_EQUAL(cbData.first, state->d_handler.getDescriptor());
      if (cbData.second.type() == typeid(std::shared_ptr<IncomingTCPConnectionState>)) {
        auto cbState = boost::any_cast<std::shared_ptr<IncomingTCPConnectionState>>(cbData.second);
        BOOST_CHECK_EQUAL(cbData.first, cbState->d_handler.getDescriptor());
        cbState->handleTimeout(cbState, false);
      }
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
  }

  {
    cerr<<"=> timeout while writing the response"<<endl;
    /* timeout while writing the response */
    s_readBuffer = query;
    s_writeBuffer.clear();
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::NeedWrite, 1 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      return ProcessQueryResult::SendAnswer;
    };

    /* mark the incoming FD as NOT ready */
    dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setNotReady(-1);

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(threadData.mplexer->run(&now), 0U);
    struct timeval later = now;
    later.tv_sec += g_tcpRecvTimeout + 1;
    auto expiredWriteConns = threadData.mplexer->getTimeouts(later, true);
    for (const auto& cbData : expiredWriteConns) {
      BOOST_CHECK_EQUAL(cbData.first, state->d_handler.getDescriptor());
      if (cbData.second.type() == typeid(std::shared_ptr<IncomingTCPConnectionState>)) {
        auto cbState = boost::any_cast<std::shared_ptr<IncomingTCPConnectionState>>(cbData.second);
        BOOST_CHECK_EQUAL(cbData.first, cbState->d_handler.getDescriptor());
        cbState->handleTimeout(cbState, false);
      }
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 1U);
  }

}

BOOST_AUTO_TEST_CASE(test_IncomingConnectionWithProxyProtocol_SelfAnswered)
{
  ComboAddress local("192.0.2.1:80");
  ClientState localCS(local, true, false, false, "", {});
  auto tlsCtx = std::make_shared<MockupTLSCtx>();
  localCS.tlsFrontend = std::make_shared<TLSFrontend>(tlsCtx);

  TCPClientThreadData threadData;
  threadData.mplexer = std::make_unique<MockupFDMultiplexer>();

  struct timeval now;
  gettimeofday(&now, nullptr);

  PacketBuffer query;
  GenericDNSPacketWriter<PacketBuffer> pwQ(query, DNSName("powerdns.com."), QType::A, QClass::IN, 0);
  pwQ.getHeader()->rd = 1;

  uint16_t querySize = static_cast<uint16_t>(query.size());
  const uint8_t sizeBytes[] = { static_cast<uint8_t>(querySize / 256), static_cast<uint8_t>(querySize % 256) };
  query.insert(query.begin(), sizeBytes, sizeBytes + 2);

  g_proxyProtocolACL.clear();
  g_proxyProtocolACL.addMask("0.0.0.0/0");

  {
    cerr<<"=> reading PP"<<endl;
    /* reading a proxy protocol payload */
    auto proxyPayload = makeProxyHeader(true, ComboAddress("192.0.2.1"), ComboAddress("192.0.2.2"), {});
    BOOST_REQUIRE_GT(proxyPayload.size(), s_proxyProtocolMinimumHeaderSize);
    s_readBuffer = query;
    // preprend the proxy protocol payload
    s_readBuffer.insert(s_readBuffer.begin(), proxyPayload.begin(), proxyPayload.end());
    // append a second query
    s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
    s_writeBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, s_proxyProtocolMinimumHeaderSize },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, proxyPayload.size() - s_proxyProtocolMinimumHeaderSize },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, 65537 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, 65537 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      return ProcessQueryResult::SendAnswer;
    };

    /* mark the incoming FD as NOT ready */
    dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setNotReady(-1);

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(threadData.mplexer->run(&now), 0U);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size() * 2U);
  }

  {
    cerr<<"=> Invalid PP"<<endl;
    /* reading a (broken) proxy protocol payload */
    auto proxyPayload = std::vector<uint8_t>(s_proxyProtocolMinimumHeaderSize);
    std::fill(proxyPayload.begin(), proxyPayload.end(), 0);

    s_readBuffer = query;
    // preprend the proxy protocol payload
    s_readBuffer.insert(s_readBuffer.begin(), proxyPayload.begin(), proxyPayload.end());
    s_writeBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, s_proxyProtocolMinimumHeaderSize },
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      return ProcessQueryResult::SendAnswer;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);

    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
  }

  {
    cerr<<"=> timeout while reading PP"<<endl;
    /* timeout while reading the proxy protocol payload */
    auto proxyPayload = makeProxyHeader(true, ComboAddress("192.0.2.1"), ComboAddress("192.0.2.2"), {});
    BOOST_REQUIRE_GT(proxyPayload.size(), s_proxyProtocolMinimumHeaderSize);
    s_readBuffer = query;
    // preprend the proxy protocol payload
    s_readBuffer.insert(s_readBuffer.begin(), proxyPayload.begin(), proxyPayload.end());
    // append a second query
    s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
    s_writeBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, s_proxyProtocolMinimumHeaderSize },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::NeedRead, proxyPayload.size() - s_proxyProtocolMinimumHeaderSize - 1},
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    s_processQuery = [](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      return ProcessQueryResult::SendAnswer;
    };

    /* mark the incoming FD as NOT ready */
    dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setNotReady(-1);

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(threadData.mplexer->run(&now), 0U);
    struct timeval later = now;
    later.tv_sec += g_tcpRecvTimeout + 1;
    auto expiredReadConns = threadData.mplexer->getTimeouts(later, false);
    for (const auto& cbData : expiredReadConns) {
      BOOST_CHECK_EQUAL(cbData.first, state->d_handler.getDescriptor());
      if (cbData.second.type() == typeid(std::shared_ptr<IncomingTCPConnectionState>)) {
        auto cbState = boost::any_cast<std::shared_ptr<IncomingTCPConnectionState>>(cbData.second);
        BOOST_CHECK_EQUAL(cbData.first, cbState->d_handler.getDescriptor());
        cbState->handleTimeout(cbState, false);
      }
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
  }
}

BOOST_AUTO_TEST_CASE(test_IncomingConnection_BackendNoOOOR)
{
  ComboAddress local("192.0.2.1:80");
  ClientState localCS(local, true, false, false, "", {});
  auto tlsCtx = std::make_shared<MockupTLSCtx>();
  localCS.tlsFrontend = std::make_shared<TLSFrontend>(tlsCtx);

  TCPClientThreadData threadData;
  threadData.mplexer = std::make_unique<MockupFDMultiplexer>();

  struct timeval now;
  gettimeofday(&now, nullptr);

  PacketBuffer query;
  GenericDNSPacketWriter<PacketBuffer> pwQ(query, DNSName("powerdns.com."), QType::A, QClass::IN, 0);
  pwQ.getHeader()->rd = 1;

  uint16_t querySize = static_cast<uint16_t>(query.size());
  const uint8_t sizeBytes[] = { static_cast<uint8_t>(querySize / 256), static_cast<uint8_t>(querySize % 256) };
  query.insert(query.begin(), sizeBytes, sizeBytes + 2);

  g_proxyProtocolACL.clear();

  {
    /* pass to backend, backend answers right away, client closes the connection */
    cerr<<"=> Query to backend, backend answers right away"<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer = query;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* closing a connection to the backend */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    s_processQuery = [tlsCtx](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
      selectedBackend->d_tlsCtx = tlsCtx;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size());
    BOOST_CHECK(s_writeBuffer == query);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size());
    BOOST_CHECK(s_backendWriteBuffer == query);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* pass to backend, backend answers right away, exception while handling the response */
    cerr<<"=> Exception while handling the response sent by the backend"<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer = query;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* closing a connection to the backend */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    s_processQuery = [tlsCtx](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
      selectedBackend->d_tlsCtx = tlsCtx;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      throw std::runtime_error("Unexpected error while processing the response");
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size());
    BOOST_CHECK(s_backendWriteBuffer == query);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* pass to backend, backend answers right away, processResponse() fails */
    cerr<<"=> Response processing fails "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer = query;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* closing a connection to the backend */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    s_processQuery = [tlsCtx](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
      selectedBackend->d_tlsCtx = tlsCtx;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return false;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size());
    BOOST_CHECK(s_backendWriteBuffer == query);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* pass to backend, backend answers right away, ID matching fails */
    cerr<<"=> ID matching fails "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    auto response = query;
    /* mess with the transaction ID */
    response.at(3) ^= 42;

    s_backendReadBuffer = response;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* closing a connection to the backend */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    s_processQuery = [tlsCtx](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
      selectedBackend->d_tlsCtx = tlsCtx;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size());
    BOOST_CHECK(s_backendWriteBuffer == query);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* connect in progress, short write to the backend, short read from the backend, client */
    cerr<<"=> Short read and write to backend"<<endl;
    s_readBuffer = query;
    // append a second query
    s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
    s_writeBuffer.clear();

    s_backendReadBuffer = query;
    // append a second query
    s_backendReadBuffer.insert(s_backendReadBuffer.end(), query.begin(), query.end());
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* connect to backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::NeedWrite, 0, [&threadData](int desc, const ExpectedStep& step) {
          /* set the outgoing descriptor (backend connection) as ready */
          dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setReady(desc);
        }
      },
      /* send query */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::NeedWrite, 1 },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() - 1 },
      /* read response */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 1 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 1 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, query.size() - 3 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 1 },
      /* write response to client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::NeedWrite, query.size() - 1 },
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, 1 },
      /* read second query */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* write second query to backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      /* read second response */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      /* write second response */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() },
      /* read from client */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      /* close connection to client */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* close connection to the backend, eventually */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };

    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    /* set the incoming descriptor as ready! */
    dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setReady(-1);
    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    while (threadData.mplexer->getWatchedFDCount(false) != 0 || threadData.mplexer->getWatchedFDCount(true) != 0) {
      threadData.mplexer->run(&now);
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size() * 2U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size() * 2U);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* connection refused by the backend */
    cerr<<"=> Connection refused by the backend "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend (5 tries by default) */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done, 0, [](int descriptor, const ExpectedStep& step) {
          throw NetworkError("Connection refused by the backend");
        }
      },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done, 0, [](int descriptor, const ExpectedStep& step) {
          throw NetworkError("Connection refused by the backend");
        }
      },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done, 0, [](int descriptor, const ExpectedStep& step) {
          throw NetworkError("Connection refused by the backend");
        }
      },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done, 0, [](int descriptor, const ExpectedStep& step) {
          throw NetworkError("Connection refused by the backend");
        }
      },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done, 0, [](int descriptor, const ExpectedStep& step) {
          throw NetworkError("Connection refused by the backend");
        }
      },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), 0U);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* timeout from the backend (write) */
    cerr<<"=> Timeout from the backend (write) "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend (retrying 5 times) */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::NeedWrite },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    struct timeval later = now;
    later.tv_sec += backend->tcpSendTimeout + 1;
    auto expiredWriteConns = threadData.mplexer->getTimeouts(later, true);
    BOOST_CHECK_EQUAL(expiredWriteConns.size(), 1U);
    for (const auto& cbData : expiredWriteConns) {
      if (cbData.second.type() == typeid(std::shared_ptr<TCPConnectionToBackend>)) {
        auto cbState = boost::any_cast<std::shared_ptr<TCPConnectionToBackend>>(cbData.second);
        cbState->handleTimeout(later, true);
      }
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), 0U);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* timeout from the backend (read) */
    cerr<<"=> Timeout from the backend (read) "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    struct timeval later = now;
    later.tv_sec += backend->tcpRecvTimeout + 1;
    auto expiredConns = threadData.mplexer->getTimeouts(later, false);
    BOOST_CHECK_EQUAL(expiredConns.size(), 1U);
    for (const auto& cbData : expiredConns) {
      if (cbData.second.type() == typeid(std::shared_ptr<TCPConnectionToBackend>)) {
        auto cbState = boost::any_cast<std::shared_ptr<TCPConnectionToBackend>>(cbData.second);
        cbState->handleTimeout(later, false);
      }
    }
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size());
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* connection closed from the backend (write) */
    cerr<<"=> Connection closed from the backend (write) "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend, connection closed on first write (5 attempts) */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), 0U);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* connection closed from the backend (write) 4 times then succeeds */
    cerr<<"=> Connection closed from the backend (write) 4 times then succeeds"<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer = query;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend, connection closed on first write (5 attempts) */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      /* reading the response */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      /* send the response to the client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() },
      /* client closes the connection */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* then eventually the backend one */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size());
    BOOST_CHECK(s_writeBuffer == query);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size());
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* connection closed from the backend (read) */
    cerr<<"=> Connection closed from the backend (read) "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend, connection closed on read, 5 attempts, last one succeeds */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), 0U);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size() * backend->retries);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    /* connection closed from the backend (read) 4 times then succeeds */
    cerr<<"=> Connection closed from the backend (read) 4 times then succeeds "<<endl;
    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer = query;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
      /* opening a connection to the backend, connection closed on read, 5 attempts, last one succeeds */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 0 },
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
      /* this time it works */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
      /* sending the response to the client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() },
      /* client closes the connection */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* the eventually the backend one */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };
    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size());
    BOOST_CHECK(s_writeBuffer == query);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), query.size() * backend->retries);
    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
#if 0
    /* 101 queries on the same connection, check that the maximum number of queries kicks in */
    cerr<<"=> 101 queries on the same connection"<<endl;

    g_maxTCPQueriesPerConn = 100;

    size_t count = 101;

    s_readBuffer = query;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendWriteBuffer.clear();

    s_readBuffer.clear();
    s_writeBuffer.clear();

    for (size_t idx = 0; idx < count; idx++) {
      s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
      s_backendReadBuffer.insert(s_backendReadBuffer.end(), query.begin(), query.end());
    }

    s_steps = { { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
                { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
                { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 },
                /* opening a connection to the backend */
                { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
                { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() + 2 },
                { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 },
                { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 },
                { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() + 2 }
    };

    for (size_t idx = 0; idx < count - 1; idx++) {
      /* read a new query */
      s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 });
      s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, query.size() - 2 });
      /* pass it to the backend */
      s_steps.push_back({ ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, query.size() + 2 });
      s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, 2 });
      s_steps.push_back({ ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, query.size() - 2 });
      /* send the response */
      s_steps.push_back({ ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, query.size() + 2 });
    };
    /* close the connection with the client */
    s_steps.push_back({ ExpectedStep::ExpectedRequest::closeClient, IOState::Done });
    /* eventually with the backend as well */
    s_steps.push_back({ ExpectedStep::ExpectedRequest::closeBackend, IOState::Done });

    auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);;
    backend->d_tlsCtx = tlsCtx;

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {

      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    BOOST_CHECK_EQUAL(s_writeBuffer.size(), query.size() * count);

    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();

    g_maxTCPQueriesPerConn = 0;
#endif
  }

}

BOOST_AUTO_TEST_CASE(test_IncomingConnectionOOOR_BackendOOOR)
{
  ComboAddress local("192.0.2.1:80");
  ClientState localCS(local, true, false, false, "", {});
  /* enable out-of-order on the front side */
  localCS.d_maxInFlightQueriesPerConn = 65536;

  auto tlsCtx = std::make_shared<MockupTLSCtx>();
  localCS.tlsFrontend = std::make_shared<TLSFrontend>(tlsCtx);

  auto backend = std::make_shared<DownstreamState>(ComboAddress("192.0.2.42:53"), ComboAddress("0.0.0.0:0"), 0, std::string(), 1, false);
  backend->d_tlsCtx = tlsCtx;
  /* enable out-of-order on the backend side as well */
  backend->d_maxInFlightQueriesPerConn = 65536;
  /* shorter than the client one */
  backend->tcpRecvTimeout = 1;

  TCPClientThreadData threadData;
  threadData.mplexer = std::make_unique<MockupFDMultiplexer>();

  struct timeval now;
  gettimeofday(&now, nullptr);

  std::vector<PacketBuffer> queries(5);
  std::vector<PacketBuffer> responses(5);

  size_t counter = 0;
  size_t totalQueriesSize = 0;
  for (auto& query : queries) {
    GenericDNSPacketWriter<PacketBuffer> pwQ(query, DNSName("powerdns" + std::to_string(counter) + ".com."), QType::A, QClass::IN, 0);
    pwQ.getHeader()->rd = 1;
    pwQ.getHeader()->id = counter;
    uint16_t querySize = static_cast<uint16_t>(query.size());
    const uint8_t sizeBytes[] = { static_cast<uint8_t>(querySize / 256), static_cast<uint8_t>(querySize % 256) };
    query.insert(query.begin(), sizeBytes, sizeBytes + 2);
    totalQueriesSize += query.size();
    ++counter;
  }

  counter = 0;
  size_t totalResponsesSize = 0;
  for (auto& response : responses) {
    DNSName name("powerdns" + std::to_string(counter) + ".com.");
    GenericDNSPacketWriter<PacketBuffer> pwR(response, name, QType::A, QClass::IN, 0);
    pwR.getHeader()->qr = 1;
    pwR.getHeader()->rd = 1;
    pwR.getHeader()->ra = 1;
    pwR.getHeader()->id = counter;
    pwR.startRecord(name, QType::A, 7200, QClass::IN, DNSResourceRecord::ANSWER);
    pwR.xfr32BitInt(0x01020304);
    pwR.commit();

    uint16_t responseSize = static_cast<uint16_t>(response.size());
    const uint8_t sizeBytes[] = { static_cast<uint8_t>(responseSize / 256), static_cast<uint8_t>(responseSize % 256) };
    response.insert(response.begin(), sizeBytes, sizeBytes + 2);
    totalResponsesSize += response.size();
    ++counter;
  }

  g_verbose = true;

  g_proxyProtocolACL.clear();

  {
    cerr<<"=> 5 OOOR queries to the backend, backend responds in reverse order"<<endl;
    PacketBuffer expectedWriteBuffer;
    PacketBuffer expectedBackendWriteBuffer;

    s_readBuffer.clear();
    for (const auto& query : queries) {
      s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
    }
    expectedBackendWriteBuffer = s_readBuffer;
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    for (const auto& response : responses) {
      /* reverse order */
      s_backendReadBuffer.insert(s_backendReadBuffer.begin(), response.begin(), response.end());
    }
    expectedWriteBuffer = s_backendReadBuffer;
    s_backendWriteBuffer.clear();

    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      /* reading a query from the client (1) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(0).size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(0).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (2) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(1).size() - 2 },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(1).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (3) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(2).size() - 2 },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(2).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (4) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(3).size() - 2 },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(3).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (5) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(4).size() - 2 },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(4).size() },
      /* no response ready yet, but the backend becomes ready */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0, [&threadData](int desc, const ExpectedStep& step) {
        /* set the outgoing descriptor (backend connection) as ready */
        dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setReady(desc);
      } },

      /* no more queries from the client */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::NeedRead, 0 },

      /* reading a response from the backend */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(4).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(4).size() },
      /* sending it to the client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(4).size() },

      /* reading a response from the backend */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(3).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(3).size() },
      /* sending it to the client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(3).size() },

      /* reading a response from the backend */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(2).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(2).size() },
      /* sending it to the client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(2).size() },

      /* reading a response from the backend */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(1).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(1).size() },
      /* sending it to the client */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(1).size() },

      /* reading a response from the backend */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(0).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(0).size() },
      /* sending it to the client, the client descriptor becomes ready */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(0).size(), [&threadData](int desc, const ExpectedStep& step) {
        /* set the incoming descriptor (client connection) as ready */
        dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setReady(desc);
      } },

      /* client is closing the connection */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 0 },
      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },
      /* closing a connection to the backend */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };

    s_processQuery = [backend](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);
    while (threadData.mplexer->getWatchedFDCount(false) != 0 || threadData.mplexer->getWatchedFDCount(true) != 0) {
      threadData.mplexer->run(&now);
    }

    BOOST_CHECK_EQUAL(s_writeBuffer.size(), totalResponsesSize);
    BOOST_CHECK(s_writeBuffer == expectedWriteBuffer);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), totalQueriesSize);
    BOOST_CHECK(s_backendWriteBuffer == expectedBackendWriteBuffer);

    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();
  }

  {
    cerr<<"=> 3 queries sent to the backend, 1 self-answered, 1 new query sent to the backend which responds to the first query right away, then to the last one, then the connection to the backend times out"<<endl;

    // increase the client timeout for that test, we want the backend to timeout first
    g_tcpRecvTimeout = 5;

    PacketBuffer expectedWriteBuffer;
    PacketBuffer expectedBackendWriteBuffer;

    s_readBuffer.clear();
    for (const auto& query : queries) {
      s_readBuffer.insert(s_readBuffer.end(), query.begin(), query.end());
    }
    s_writeBuffer.clear();

    s_backendReadBuffer.clear();
    s_backendReadBuffer.insert(s_backendReadBuffer.end(), responses.at(0).begin(), responses.at(0).end());
    s_backendReadBuffer.insert(s_backendReadBuffer.end(), responses.at(4).begin(), responses.at(4).end());

    /* self-answered */
    expectedWriteBuffer.insert(expectedWriteBuffer.end(), responses.at(3).begin(), responses.at(3).end());
    /* from backend */
    expectedWriteBuffer.insert(expectedWriteBuffer.end(), responses.at(0).begin(), responses.at(0).end());
    expectedWriteBuffer.insert(expectedWriteBuffer.end(), responses.at(4).begin(), responses.at(4).end());

    s_backendWriteBuffer.clear();

    expectedBackendWriteBuffer.insert(expectedBackendWriteBuffer.end(), queries.at(0).begin(), queries.at(0).end());
    expectedBackendWriteBuffer.insert(expectedBackendWriteBuffer.end(), queries.at(1).begin(), queries.at(1).end());
    expectedBackendWriteBuffer.insert(expectedBackendWriteBuffer.end(), queries.at(2).begin(), queries.at(2).end());
    expectedBackendWriteBuffer.insert(expectedBackendWriteBuffer.end(), queries.at(4).begin(), queries.at(4).end());


    bool timeout = false;
    s_steps = {
      { ExpectedStep::ExpectedRequest::handshakeClient, IOState::Done },
      /* reading a query from the client (1) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(0).size() - 2 },
      /* opening a connection to the backend */
      { ExpectedStep::ExpectedRequest::connectToBackend, IOState::Done },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(0).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (2) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(1).size() - 2 },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(1).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (3) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(2).size() - 2 },
      /* sending query to the backend */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(2).size() },
      /* no response ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0 },
      /* reading a query from the client (4) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(3).size() - 2 },
      /* sending the response right away (self-answered)  */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(3).size() },
      /* reading a query from the client (5) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, 2 },
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::Done, queries.at(4).size() - 2 },
      /* sending query to the backend (5) */
      { ExpectedStep::ExpectedRequest::writeToBackend, IOState::Done, queries.at(4).size() },
      /* reading a response from the backend (1) */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(0).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(0).size(), [&threadData](int desc, const ExpectedStep& step) {
        /* set the backend descriptor as ready */
        dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setReady(desc);
      } },
      /* sending it to the client (1) */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(0).size(), [&threadData](int desc, const ExpectedStep& step) {
        /* set the client descriptor as NOT ready */
        dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setNotReady(desc);
      } },
      /* reading from the client (not ready) */
      { ExpectedStep::ExpectedRequest::readFromClient, IOState::NeedRead, 0 },
      /* reading a response from the backend (5) */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(4).size() - 2 },
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::Done, responses.at(4).size() },
      /* sending it to the client (5) */
      { ExpectedStep::ExpectedRequest::writeToClient, IOState::Done, responses.at(4).size() },

      /* try to read from the backend but there is no answer ready yet */
      { ExpectedStep::ExpectedRequest::readFromBackend, IOState::NeedRead, 0, [&threadData, &timeout](int desc, const ExpectedStep& step) {
        /* set the backend descriptor as NOT ready */
        dynamic_cast<MockupFDMultiplexer*>(threadData.mplexer.get())->setNotReady(desc);
        timeout = true;
      } },

      /* A timeout occurs */

      /* closing client connection */
      { ExpectedStep::ExpectedRequest::closeClient, IOState::Done },

      /* closing a connection to the backend */
      { ExpectedStep::ExpectedRequest::closeBackend, IOState::Done },
    };

    s_processQuery = [backend,&responses](DNSQuestion& dq, ClientState& cs, LocalHolders& holders, std::shared_ptr<DownstreamState>& selectedBackend) -> ProcessQueryResult {
      static size_t count = 0;
      if (count++ == 3) {
        /* self answered */
        dq.getMutableData() = responses.at(3);
        /* remove the length */
        dq.getMutableData().erase(dq.getMutableData().begin(), dq.getMutableData().begin() + 2);

        return ProcessQueryResult::SendAnswer;
      }
      selectedBackend = backend;
      return ProcessQueryResult::PassToBackend;
    };
    s_processResponse = [](PacketBuffer& response, LocalStateHolder<vector<DNSDistResponseRuleAction> >& localRespRulactions, DNSResponse& dr, bool muted) -> bool {
      return true;
    };

    auto state = std::make_shared<IncomingTCPConnectionState>(ConnectionInfo(&localCS), threadData, now);
    IncomingTCPConnectionState::handleIO(state, now);

    while (!timeout && (threadData.mplexer->getWatchedFDCount(false) != 0 || threadData.mplexer->getWatchedFDCount(true) != 0)) {
      threadData.mplexer->run(&now);
    }

    struct timeval later = now;
    later.tv_sec += backend->tcpRecvTimeout + 1;
    auto expiredConns = threadData.mplexer->getTimeouts(later, false);
    BOOST_CHECK_EQUAL(expiredConns.size(), 1U);
    for (const auto& cbData : expiredConns) {
      if (cbData.second.type() == typeid(std::shared_ptr<TCPConnectionToBackend>)) {
        auto cbState = boost::any_cast<std::shared_ptr<TCPConnectionToBackend>>(cbData.second);
        cbState->handleTimeout(later, false);
      }
    }

    BOOST_CHECK_EQUAL(s_writeBuffer.size(), expectedWriteBuffer.size());
    BOOST_CHECK(s_writeBuffer == expectedWriteBuffer);
    BOOST_CHECK_EQUAL(s_backendWriteBuffer.size(), expectedBackendWriteBuffer.size());
    BOOST_CHECK(s_backendWriteBuffer == expectedBackendWriteBuffer);

    /* we need to clear them now, otherwise we end up with dangling pointers to the steps via the TLS context, etc */
    IncomingTCPConnectionState::clearAllDownstreamConnections();

    // restore the client timeout
    g_tcpRecvTimeout = 2;
  }
}

#warning TODO:

// OOOR: OOOR enabled but packet cache hit
// OOOR: OOOR enabled but backend answers very fast
// OOOR: OOOR, get 10 queries before the backend can answer. backend doesn't support OOOR, we should get 10 connections. Check that we do reuse them on two subsequent queries
// OOOR: OOOR, get 10 queries before the backend can answer. backend does support OOOR, respond out of order we should only have 1 connections. Check that we do reuse them on two subsequent queries
// OOOR: OOOR, get 10 queries before the backend can answer. backend does support OOOR but only up to 5 conns, respond out of order, we should only have 2 connections. Check that we do reuse one of them on two subsequent queries
// OOOR: get one query, sent it to the backend, start reading the response, get two new queries during that time, finish getting the first answer, send it, timeout read on the client, get the last two answers and send them
// out-of-order query from cache while pending response (short write) from backend, exception while processing the response

BOOST_AUTO_TEST_SUITE_END();
