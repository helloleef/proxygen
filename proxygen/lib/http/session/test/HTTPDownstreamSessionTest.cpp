/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/Foreach.h>
#include <folly/experimental/wangle/ConnectionManager.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/TimeoutManager.h>
#include <gtest/gtest.h>
#include <proxygen/lib/http/codec/test/MockHTTPCodec.h>
#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <proxygen/lib/http/session/HTTPDirectResponseHandler.h>
#include <proxygen/lib/http/session/HTTPDownstreamSession.h>
#include <proxygen/lib/http/session/HTTPSession.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPSessionTest.h>
#include <proxygen/lib/http/session/test/TestUtils.h>
#include <proxygen/lib/test/TestAsyncTransport.h>
#include <string>
#include <strstream>
#include <thrift/lib/cpp/test/MockTAsyncTransport.h>
#include <vector>

using namespace apache::thrift::async;
using namespace apache::thrift::test;
using namespace apache::thrift::transport;
using namespace folly::wangle;
using namespace folly;
using namespace proxygen;
using namespace std;
using namespace testing;

struct HTTP1xCodecPair {
  typedef HTTP1xCodec Codec;
  static const int version = 1;
};

struct SPDY2CodecPair {
  typedef SPDYCodec Codec;
  static const SPDYVersion version = SPDYVersion::SPDY2;
};

struct SPDY3CodecPair {
  typedef SPDYCodec Codec;
  static const SPDYVersion version = SPDYVersion::SPDY3;
};

struct SPDY3_1CodecPair {
  typedef SPDYCodec Codec;
  static const SPDYVersion version = SPDYVersion::SPDY3_1;
};

template <typename C>
class HTTPDownstreamTest : public testing::Test {
 public:
  explicit HTTPDownstreamTest(uint32_t sessionWindowSize = spdy::kInitialWindow)
    : eventBase_(),
      transport_(new TestAsyncTransport(&eventBase_)),
      transactionTimeouts_(makeTimeoutSet(&eventBase_)) {
    EXPECT_CALL(mockController_, attachSession(_));
    httpSession_ = new HTTPDownstreamSession(
      transactionTimeouts_.get(),
      std::move(TAsyncTransport::UniquePtr(transport_)),
      localAddr, peerAddr,
      &mockController_,
      std::move(makeServerCodec<typename C::Codec>(
                  C::version)),
      mockTransportInfo /* no stats for now */);
    httpSession_->setFlowControl(spdy::kInitialWindow, spdy::kInitialWindow,
                                 sessionWindowSize);
    httpSession_->startNow();
  }

  void SetUp() {
  }

  void addSingleByteReads(const char* data,
                          std::chrono::milliseconds delay={}) {
    for (const char* p = data; *p != '\0'; ++p) {
      transport_->addReadEvent(p, 1, delay);
    }
  }

  void testPriorities(HTTPCodec& clientCodec, uint32_t numPriorities);

  void testChunks(bool trailers);

  void parseOutput(HTTPCodec& clientCodec) {
    IOBufQueue stream(IOBufQueue::cacheChainLength());
    auto writeEvents = transport_->getWriteEvents();
    for (auto event: *writeEvents) {
      auto vec = event->getIoVec();
      for (auto i = 0; i < event->getCount(); i++) {
        unique_ptr<IOBuf> buf(
          std::move(IOBuf::wrapBuffer(vec[i].iov_base, vec[i].iov_len)));
        stream.append(std::move(buf));
        uint32_t consumed = clientCodec.onIngress(*stream.front());
        stream.split(consumed);
      }
    }
    EXPECT_EQ(stream.chainLength(), 0);
  }
 protected:
  EventBase eventBase_;
  TestAsyncTransport* transport_;  // invalid once httpSession_ is destroyed
  AsyncTimeoutSet::UniquePtr transactionTimeouts_;
  StrictMock<MockController> mockController_;
  HTTPDownstreamSession* httpSession_;
};

// Uses TestAsyncTransport
typedef HTTPDownstreamTest<HTTP1xCodecPair> HTTPDownstreamSessionTest;
typedef HTTPDownstreamTest<SPDY2CodecPair> SPDY2DownstreamSessionTest;
typedef HTTPDownstreamTest<SPDY3CodecPair> SPDY3DownstreamSessionTest;

TEST_F(HTTPDownstreamSessionTest, immediate_eof) {
  // Send EOF without any request data
  EXPECT_CALL(mockController_, getRequestHandler(_, _)).Times(0);
  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, http_1_0_no_headers) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler->txn_));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          EXPECT_FALSE(msg->getIsChunked());
          EXPECT_FALSE(msg->getIsUpgraded());
          EXPECT_EQ("/", msg->getURL());
          EXPECT_EQ("/", msg->getPath());
          EXPECT_EQ("", msg->getQueryString());
          EXPECT_EQ(1, msg->getHTTPVersion().first);
          EXPECT_EQ(0, msg->getHTTPVersion().second);
        }));
  EXPECT_CALL(*handler, onEOM())
    .WillOnce(InvokeWithoutArgs(handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("GET / HTTP/1.0\r\n\r\n",
                           std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, http_1_0_no_headers_eof) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler->txn_));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          EXPECT_FALSE(msg->getIsChunked());
          EXPECT_FALSE(msg->getIsUpgraded());
          EXPECT_EQ("http://example.com/foo?bar", msg->getURL());
          EXPECT_EQ("/foo", msg->getPath());
          EXPECT_EQ("bar", msg->getQueryString());
          EXPECT_EQ(1, msg->getHTTPVersion().first);
          EXPECT_EQ(0, msg->getHTTPVersion().second);
        }));
  EXPECT_CALL(*handler, onEOM())
    .WillOnce(InvokeWithoutArgs(handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("GET http://example.com/foo?bar HTTP/1.0\r\n\r\n",
                           std::chrono::milliseconds(0));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, single_bytes) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;
  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler->txn_));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          const HTTPHeaders& hdrs = msg->getHeaders();
          EXPECT_EQ(2, hdrs.size());
          EXPECT_TRUE(hdrs.exists("host"));
          EXPECT_TRUE(hdrs.exists("connection"));

          EXPECT_FALSE(msg->getIsChunked());
          EXPECT_FALSE(msg->getIsUpgraded());
          EXPECT_EQ("/somepath.php?param=foo", msg->getURL());
          EXPECT_EQ("/somepath.php", msg->getPath());
          EXPECT_EQ("param=foo", msg->getQueryString());
          EXPECT_EQ(1, msg->getHTTPVersion().first);
          EXPECT_EQ(1, msg->getHTTPVersion().second);
        }));
  EXPECT_CALL(*handler, onEOM())
    .WillOnce(InvokeWithoutArgs(handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  addSingleByteReads("GET /somepath.php?param=foo HTTP/1.1\r\n"
                     "Host: example.com\r\n"
                     "Connection: close\r\n"
                     "\r\n");
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, single_bytes_with_body) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;
  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler->txn_));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          const HTTPHeaders& hdrs = msg->getHeaders();
          EXPECT_EQ(3, hdrs.size());
          EXPECT_TRUE(hdrs.exists("host"));
          EXPECT_TRUE(hdrs.exists("content-length"));
          EXPECT_TRUE(hdrs.exists("myheader"));

          EXPECT_FALSE(msg->getIsChunked());
          EXPECT_FALSE(msg->getIsUpgraded());
          EXPECT_EQ("/somepath.php?param=foo", msg->getURL());
          EXPECT_EQ("/somepath.php", msg->getPath());
          EXPECT_EQ("param=foo", msg->getQueryString());
          EXPECT_EQ(1, msg->getHTTPVersion().first);
          EXPECT_EQ(1, msg->getHTTPVersion().second);
        }));
  EXPECT_CALL(*handler, onBody(_))
    .WillOnce(ExpectString("1"))
    .WillOnce(ExpectString("2"))
    .WillOnce(ExpectString("3"))
    .WillOnce(ExpectString("4"))
    .WillOnce(ExpectString("5"));
  EXPECT_CALL(*handler, onEOM())
    .WillOnce(InvokeWithoutArgs(handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  addSingleByteReads("POST /somepath.php?param=foo HTTP/1.1\r\n"
                     "Host: example.com\r\n"
                     "MyHeader: FooBar\r\n"
                     "Content-Length: 5\r\n"
                     "\r\n"
                     "12345");
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, split_body) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;
  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler->txn_));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          const HTTPHeaders& hdrs = msg->getHeaders();
          EXPECT_EQ(2, hdrs.size());
        }));
  EXPECT_CALL(*handler, onBody(_))
    .WillOnce(ExpectString("12345"))
    .WillOnce(ExpectString("abcde"));
  EXPECT_CALL(*handler, onEOM())
    .WillOnce(InvokeWithoutArgs(handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("POST / HTTP/1.1\r\n"
                           "Host: example.com\r\n"
                           "Content-Length: 10\r\n"
                           "\r\n"
                           "12345", std::chrono::milliseconds(0));
  transport_->addReadEvent("abcde", std::chrono::milliseconds(5));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, post_chunked) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;
  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler->txn_));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          const HTTPHeaders& hdrs = msg->getHeaders();
          EXPECT_EQ(3, hdrs.size());
          EXPECT_TRUE(hdrs.exists("host"));
          EXPECT_TRUE(hdrs.exists("content-type"));
          EXPECT_TRUE(hdrs.exists("transfer-encoding"));
          EXPECT_TRUE(msg->getIsChunked());
          EXPECT_FALSE(msg->getIsUpgraded());
          EXPECT_EQ("http://example.com/cgi-bin/foo.aspx?abc&def",
                    msg->getURL());
          EXPECT_EQ("/cgi-bin/foo.aspx", msg->getPath());
          EXPECT_EQ("abc&def", msg->getQueryString());
          EXPECT_EQ(1, msg->getHTTPVersion().first);
          EXPECT_EQ(1, msg->getHTTPVersion().second);
        }));
  EXPECT_CALL(*handler, onChunkHeader(3));
  EXPECT_CALL(*handler, onBody(_))
    .WillOnce(ExpectString("bar"));
  EXPECT_CALL(*handler, onChunkComplete());
  EXPECT_CALL(*handler, onChunkHeader(0x22));
  EXPECT_CALL(*handler, onBody(_))
    .WillOnce(ExpectString("0123456789abcdef\nfedcba9876543210\n"));
  EXPECT_CALL(*handler, onChunkComplete());
  EXPECT_CALL(*handler, onChunkHeader(3));
  EXPECT_CALL(*handler, onBody(_))
    .WillOnce(ExpectString("foo"));
  EXPECT_CALL(*handler, onChunkComplete());
  EXPECT_CALL(*handler, onEOM())
    .WillOnce(InvokeWithoutArgs(handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("POST http://example.com/cgi-bin/foo.aspx?abc&def "
                           "HTTP/1.1\r\n"
                           "Host: example.com\r\n"
                           "Content-Type: text/pla", std::chrono::milliseconds(0));
  transport_->addReadEvent("in; charset=utf-8\r\n"
                           "Transfer-encoding: chunked\r\n"
                           "\r", std::chrono::milliseconds(2));
  transport_->addReadEvent("\n"
                           "3\r\n"
                           "bar\r\n"
                           "22\r\n"
                           "0123456789abcdef\n"
                           "fedcba9876543210\n"
                           "\r\n"
                           "3\r", std::chrono::milliseconds(3));
  transport_->addReadEvent("\n"
                           "foo\r\n"
                           "0\r\n\r\n", std::chrono::milliseconds(1));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, multi_message) {
  MockHTTPHandler* handler1 = new MockHTTPHandler();
  MockHTTPHandler* handler2 = new MockHTTPHandler();

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler1))
    .WillOnce(Return(handler2));

  InSequence dummy;
  EXPECT_CALL(*handler1, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler1->txn_));
  EXPECT_CALL(*handler1, onHeadersComplete(_));
  EXPECT_CALL(*handler1, onBody(_))
    .WillOnce(ExpectString("foo"))
    .WillOnce(ExpectString("bar9876"));
  EXPECT_CALL(*handler1, onEOM())
    .WillOnce(InvokeWithoutArgs(handler1, &MockHTTPHandler::sendReply));
  EXPECT_CALL(*handler1, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler1; }));

  EXPECT_CALL(*handler2, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler2->txn_));
  EXPECT_CALL(*handler2, onHeadersComplete(_));
  EXPECT_CALL(*handler2, onChunkHeader(0xa));
  EXPECT_CALL(*handler2, onBody(_))
    .WillOnce(ExpectString("some "))
    .WillOnce(ExpectString("data\n"));
  EXPECT_CALL(*handler2, onChunkComplete());
  EXPECT_CALL(*handler2, onEOM())
    .WillOnce(InvokeWithoutArgs(handler2, &MockHTTPHandler::terminate));
  EXPECT_CALL(*handler2, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler2; }));

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("POST / HTTP/1.1\r\n"
                           "Host: example.com\r\n"
                           "Content-Length: 10\r\n"
                           "\r\n"
                           "foo", std::chrono::milliseconds(0));
  transport_->addReadEvent("bar9876"
                           "POST /foo HTTP/1.1\r\n"
                           "Host: exa", std::chrono::milliseconds(2));
  transport_->addReadEvent("mple.com\r\n"
                           "Connection: close\r\n"
                           "Trans", std::chrono::milliseconds(0));
  transport_->addReadEvent("fer-encoding: chunked\r\n"
                           "\r\n", std::chrono::milliseconds(2));
  transport_->addReadEvent("a\r\nsome ", std::chrono::milliseconds(0));
  transport_->addReadEvent("data\n\r\n0\r\n\r\n", std::chrono::milliseconds(2));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, connect) {
  StrictMock<MockHTTPHandler> handler;

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  // Send HTTP 200 OK to accept the CONNECT request
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&handler] (std::shared_ptr<HTTPMessage> msg) {
          handler.sendHeaders(200, 100);
        }));

  EXPECT_CALL(handler, onUpgrade(_));

  // Data should be received using onBody
  EXPECT_CALL(handler, onBody(_))
    .WillOnce(ExpectString("12345"))
    .WillOnce(ExpectString("abcde"));
  EXPECT_CALL(handler, onEOM())
    .WillOnce(InvokeWithoutArgs(&handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(handler, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("CONNECT test HTTP/1.1\r\n"
                           "\r\n"
                           "12345", std::chrono::milliseconds(0));
  transport_->addReadEvent("abcde", std::chrono::milliseconds(5));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, connect_rejected) {
  StrictMock<MockHTTPHandler> handler;

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  // Send HTTP 400 to reject the CONNECT request
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&handler] (std::shared_ptr<HTTPMessage> msg) {
          handler.sendReplyCode(400);
        }));

  EXPECT_CALL(handler, onEOM())
    .WillOnce(InvokeWithoutArgs(&handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(handler, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("CONNECT test HTTP/1.1\r\n"
                           "\r\n"
                           "12345", std::chrono::milliseconds(0));
  transport_->addReadEvent("abcde", std::chrono::milliseconds(5));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, http_upgrade) {
  StrictMock<MockHTTPHandler> handler;

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  // Send HTTP 101 Switching Protocls to accept the upgrade request
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&handler] (std::shared_ptr<HTTPMessage> msg) {
          handler.sendHeaders(101, 100);
        }));

  // Send the response in the new protocol after upgrade
  EXPECT_CALL(handler, onUpgrade(_))
    .WillOnce(Invoke([&handler] (UpgradeProtocol protocol) {
          handler.sendReplyCode(100);
        }));

  EXPECT_CALL(handler, onEOM())
    .WillOnce(InvokeWithoutArgs(&handler, &MockHTTPHandler::terminate));
  EXPECT_CALL(handler, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("GET /upgrade HTTP/1.1\r\n"
                           "Upgrade: TEST/1.0\r\n"
                           "Connection: upgrade\r\n"
                           "\r\n", std::chrono::milliseconds(0));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST(HTTPDownstreamTest, parse_error_no_txn) {
  // 1) Get a parse error on SYN_STREAM for streamID == 1
  // 2) Expect that the codec should be asked to generate an abort on
  //    streamID==1
  EventBase evb;

  // Setup the controller and its expecations.
  NiceMock<MockController> mockController;

  // Setup the codec, its callbacks, and its expectations.
  auto codec = makeDownstreamParallelCodec();
  HTTPCodec::Callback* codecCallback = nullptr;
  EXPECT_CALL(*codec, setCallback(_))
    .WillRepeatedly(SaveArg<0>(&codecCallback));
  // Expect egress abort for streamID == 1
  EXPECT_CALL(*codec, generateRstStream(_, 1, _));

  // Setup transport
  bool transportGood = true;
  auto transport = newMockTransport(&evb);
  EXPECT_CALL(*transport, good())
    .WillRepeatedly(ReturnPointee(&transportGood));
  EXPECT_CALL(*transport, closeNow())
    .WillRepeatedly(Assign(&transportGood, false));
  EXPECT_CALL(*transport, writeChain(_, _, _))
    .WillRepeatedly(Invoke([&] (TAsyncTransport::WriteCallback* callback,
                                const shared_ptr<IOBuf> iob,
                                WriteFlags flags) {
                             callback->writeSuccess();
                           }));

  // Create the downstream session, thus initializing codecCallback
  auto transactionTimeouts = makeInternalTimeoutSet(&evb);
  auto session = new HTTPDownstreamSession(
    transactionTimeouts.get(),
    TAsyncTransport::UniquePtr(transport),
    localAddr, peerAddr,
    &mockController, std::move(codec),
    mockTransportInfo);
  session->startNow();
  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS, "foo");
  ex.setProxygenError(kErrorParseHeader);
  ex.setCodecStatusCode(ErrorCode::REFUSED_STREAM);
  codecCallback->onError(HTTPCodec::StreamID(1), ex, true);

  // cleanup
  session->shutdownTransportWithReset(kErrorConnectionReset);
  evb.loop();
}

TEST_F(HTTPDownstreamSessionTest, trailers) {
  testChunks(true);
}

TEST_F(HTTPDownstreamSessionTest, explicit_chunks) {
  testChunks(false);
}

template <class C>
void HTTPDownstreamTest<C>::testChunks(bool trailers) {
  StrictMock<MockHTTPHandler> handler;

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));
  EXPECT_CALL(handler, onHeadersComplete(_));
  EXPECT_CALL(handler, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler, trailers] () {
          handler.sendChunkedReplyWithBody(200, 100, 17, trailers);
        }));
  EXPECT_CALL(handler, detachTransaction());

  transport_->addReadEvent("GET / HTTP/1.1\r\n"
                           "\r\n", std::chrono::milliseconds(0));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  HTTPSession::DestructorGuard g(httpSession_);
  eventBase_.loop();

  HTTP1xCodec clientCodec(TransportDirection::UPSTREAM);
  NiceMock<MockHTTPCodecCallback> callbacks;

  EXPECT_CALL(callbacks, onMessageBegin(1, _))
    .Times(1);
  EXPECT_CALL(callbacks, onHeadersComplete(1, _))
    .Times(1);
  for (int i = 0; i < 6; i++) {
    EXPECT_CALL(callbacks, onChunkHeader(1, _));
    EXPECT_CALL(callbacks, onBody(1, _));
    EXPECT_CALL(callbacks, onChunkComplete(1));
  }
  if (trailers) {
    EXPECT_CALL(callbacks, onTrailersComplete(1, _));
  }
  EXPECT_CALL(callbacks, onMessageComplete(1, _));

  clientCodec.setCallback(&callbacks);
  parseOutput(clientCodec);
  EXPECT_CALL(mockController_, detachSession(_));
}

TEST_F(HTTPDownstreamSessionTest, http_drain) {
  StrictMock<MockHTTPHandler> handler1;
  StrictMock<MockHTTPHandler> handler2;

  InSequence dummy;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler1.txn_));

  EXPECT_CALL(handler1, onHeadersComplete(_))
    .WillOnce(Invoke([this, &handler1] (std::shared_ptr<HTTPMessage> msg) {
          handler1.sendHeaders(200, 100);
          httpSession_->notifyPendingShutdown();
        }));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1] {
          handler1.sendBody(100);
          handler1.txn_->sendEOM();
        }));
  EXPECT_CALL(handler1, detachTransaction());

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler2));

  EXPECT_CALL(handler2, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler2.txn_));

  EXPECT_CALL(handler2, onHeadersComplete(_))
    .WillOnce(Invoke([this, &handler2] (std::shared_ptr<HTTPMessage> msg) {
          handler2.sendHeaders(200, 100);
        }));
  EXPECT_CALL(handler2, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler2] {
          handler2.sendBody(100);
          handler2.txn_->sendEOM();
        }));
  EXPECT_CALL(handler2, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("GET / HTTP/1.1\r\n"
                           "\r\n", std::chrono::milliseconds(0));
  transport_->addReadEvent("GET / HTTP/1.1\r\n"
                           "\r\n", std::chrono::milliseconds(0));

  transport_->startReadEvents();
  eventBase_.loop();
}

// 1) receive full request
// 2) notify pending shutdown
// 3) wait for session read timeout -> should be ignored
// 4) response completed
TEST_F(HTTPDownstreamSessionTest, http_drain_long_running) {
  StrictMock<MockHTTPHandler> handler;

  InSequence enforceSequence;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  // txn1, as soon as headers go out, mark set code shouldShutdown=true
  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(SaveArg<0>(&handler.txn_));

  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([this, &handler] (std::shared_ptr<HTTPMessage> msg) {
          httpSession_->notifyPendingShutdown();
          eventBase_.runAfterDelay([this] {
              // simulate read timeout
              httpSession_->timeoutExpired();
            }, 100);
          eventBase_.runAfterDelay([&handler] {
              handler.sendReplyWithBody(200, 100);
            }, 200);
        }));
  EXPECT_CALL(handler, onEOM());
  EXPECT_CALL(handler, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent("GET / HTTP/1.1\r\n"
                           "\r\n", std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, early_abort) {
  MockHTTPHandler* handler = new MockHTTPHandler();

  InSequence dummy;
  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler));

  EXPECT_CALL(*handler, setTransaction(_))
    .WillOnce(Invoke([&] (HTTPTransaction* txn) {
          handler->txn_ = txn;
          handler->txn_->sendAbort();
        }));
  EXPECT_CALL(*handler, onHeadersComplete(_))
    .Times(0);
  EXPECT_CALL(*handler, detachTransaction())
    .WillOnce(InvokeWithoutArgs([&] { delete handler; }));

  EXPECT_CALL(mockController_, detachSession(_));

  addSingleByteReads("GET /somepath.php?param=foo HTTP/1.1\r\n"
                     "Host: example.com\r\n"
                     "Connection: close\r\n"
                     "\r\n");
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, http_writes_draining_timeout) {
  IOBufQueue requests;
  HTTPMessage req = getGetRequest();
  MockHTTPHandler handler1;
  HTTP1xCodec clientCodec(TransportDirection::UPSTREAM);
  auto streamID = HTTPCodec::StreamID(0);
  clientCodec.generateHeader(requests, streamID, req);
  clientCodec.generateEOM(requests, streamID);
  clientCodec.generateHeader(requests, streamID, req);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));
  EXPECT_CALL(mockController_, detachSession(_));

  InSequence handlerSequence;
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1, this] {
          transport_->pauseWrites();
          handler1.sendHeaders(200, 1000);
        }));
  EXPECT_CALL(handler1, onEgressPaused());
  EXPECT_CALL(handler1, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& ex) {
          ASSERT_EQ(ex.getProxygenError(), kErrorWriteTimeout);
          handler1.txn_->sendAbort();
        }));
  EXPECT_CALL(handler1, detachTransaction());

  transport_->addReadEvent(requests, std::chrono::milliseconds(10));
  transport_->startReadEvents();
  eventBase_.loop();
}

// Send a 1.0 request, egress the EOM with the last body chunk on a paused
// socket, and let it timeout.  shutdownTransportWithReset will result in a call
// to removeTransaction with writesDraining_=true
TEST_F(HTTPDownstreamSessionTest, write_timeout) {
  IOBufQueue requests;
  MockHTTPHandler handler1;
  HTTPMessage req = getGetRequest();
  req.setHTTPVersion(1, 0);
  HTTP1xCodec clientCodec(TransportDirection::UPSTREAM);
  auto streamID = HTTPCodec::StreamID(0);
  clientCodec.generateHeader(requests, streamID, req);
  clientCodec.generateEOM(requests, streamID);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  InSequence handlerSequence;
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1, this] {
          handler1.sendHeaders(200, 100);
          eventBase_.runAfterDelay([&handler1, this] {
              transport_->pauseWrites();
              handler1.sendBody(100);
              handler1.txn_->sendEOM();
            }, 50);
        }));
  EXPECT_CALL(handler1, onEgressPaused());
  EXPECT_CALL(handler1, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& ex) {
          ASSERT_EQ(ex.getProxygenError(), kErrorWriteTimeout);
        }));
  EXPECT_CALL(handler1, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent(requests, std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

// Send an abort from the write timeout path while pipelining
TEST_F(HTTPDownstreamSessionTest, write_timeout_pipeline) {
  IOBufQueue requests;
  MockHTTPHandler handler1;

  HTTPMessage req = getGetRequest();
  HTTP1xCodec clientCodec(TransportDirection::UPSTREAM);
  const char* buf = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
    "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  InSequence handlerSequence;
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1, this] {
          handler1.sendHeaders(200, 100);
          eventBase_.runAfterDelay([&handler1, this] {
              transport_->pauseWrites();
              handler1.sendBody(100);
              handler1.txn_->sendEOM();
            }, 50);
        }));
  EXPECT_CALL(handler1, onEgressPaused());
  EXPECT_CALL(handler1, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& ex) {
          ASSERT_EQ(ex.getProxygenError(), kErrorWriteTimeout);
          handler1.txn_->sendAbort();
        }));
  EXPECT_CALL(handler1, detachTransaction());
  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent(buf, std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, body_packetization) {
  IOBufQueue requests;
  MockHTTPHandler handler1;
  HTTPMessage req = getGetRequest();
  req.setHTTPVersion(1, 0);
  req.setWantsKeepalive(false);
  HTTP1xCodec clientCodec(TransportDirection::UPSTREAM);
  auto streamID = HTTPCodec::StreamID(0);
  clientCodec.generateHeader(requests, streamID, req);
  clientCodec.generateEOM(requests, streamID);

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));

  InSequence handlerSequence;
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1, this] {
          handler1.sendReplyWithBody(200, 100);
        }));
  EXPECT_CALL(handler1, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent(requests, std::chrono::milliseconds(0));
  transport_->startReadEvents();

  // Keep the session around even after the event base loop completes so we can
  // read the counters on a valid object.
  HTTPSession::DestructorGuard g(httpSession_);
  eventBase_.loop();

  EXPECT_EQ(transport_->getWriteEvents()->size(), 1);
}

TEST_F(HTTPDownstreamSessionTest, http_malformed_pkt1) {
  // Create a HTTP connection and keep sending just '\n' to the HTTP1xCodec.
  std::string data(90000, '\n');

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent(data.c_str(), data.length(),
                           std::chrono::milliseconds(0));
  transport_->addReadEOF(std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();
}

TEST_F(HTTPDownstreamSessionTest, big_explcit_chunk_write) {
  // even when the handler does a massive write, the transport only gets small
  // writes
  IOBufQueue requests;
  HTTPMessage req = getGetRequest();
  HTTP1xCodec clientCodec(TransportDirection::UPSTREAM);
  auto streamID = HTTPCodec::StreamID(0);
  clientCodec.generateHeader(requests, streamID, req);
  clientCodec.generateEOM(requests, streamID);
  MockHTTPHandler handler;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(Invoke([&handler] (HTTPTransaction* txn) {
          handler.txn_ = txn; }));
  EXPECT_CALL(handler, onHeadersComplete(_))
    .WillOnce(Invoke([&handler] (std::shared_ptr<HTTPMessage> msg) {
          handler.sendHeaders(200, 100, false);
          size_t len = 16 * 1024 * 1024;
          handler.txn_->sendChunkHeader(len);
          auto chunk = makeBuf(len);
          handler.txn_->sendBody(std::move(chunk));
          handler.txn_->sendChunkTerminator();
          handler.txn_->sendEOM();
        }));
  EXPECT_CALL(handler, onEgressPaused());
  EXPECT_CALL(handler, onEgressResumed());
  EXPECT_CALL(handler, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent(requests, std::chrono::milliseconds(0));
  transport_->startReadEvents();

  // Keep the session around even after the event base loop completes so we can
  // read the counters on a valid object.
  HTTPSession::DestructorGuard g(httpSession_);
  eventBase_.loop();

  EXPECT_GT(transport_->getWriteEvents()->size(), 3000);
}

TEST_F(SPDY2DownstreamSessionTest, spdy_prio) {
  SPDYCodec clientCodec(TransportDirection::UPSTREAM,
                        SPDYVersion::SPDY2);
  testPriorities(clientCodec, 4);
}

TEST_F(SPDY3DownstreamSessionTest, spdy_prio) {
  SPDYCodec clientCodec(TransportDirection::UPSTREAM,
                        SPDYVersion::SPDY3);
  testPriorities(clientCodec, 8);
}

template <class C>
void HTTPDownstreamTest<C>::testPriorities(
  HTTPCodec& clientCodec, uint32_t numPriorities) {
  IOBufQueue requests;
  uint32_t iterations = 10;
  uint32_t maxPriority = numPriorities - 1;
  HTTPMessage req = getGetRequest();
  auto streamID = HTTPCodec::StreamID(1);
  for (int pri = numPriorities - 1; pri >= 0; pri--) {
    req.setPriority(pri);
    for (auto i = 0; i < iterations; i++) {
      clientCodec.generateHeader(requests, streamID, req);
      clientCodec.generateEOM(requests, streamID);
      MockHTTPHandler* handler = new MockHTTPHandler();
      InSequence handlerSequence;
      EXPECT_CALL(mockController_, getRequestHandler(_, _))
        .WillOnce(Return(handler));
      EXPECT_CALL(*handler, setTransaction(_))
        .WillOnce(Invoke([handler] (HTTPTransaction* txn) {
              handler->txn_ = txn; }));
      EXPECT_CALL(*handler, onHeadersComplete(_));
      EXPECT_CALL(*handler, onEOM())
        .WillOnce(InvokeWithoutArgs([handler] {
              handler->sendReplyWithBody(200, 1000);
            }));
      EXPECT_CALL(*handler, detachTransaction())
        .WillOnce(InvokeWithoutArgs([handler] { delete handler; }));
      streamID += 2;
    }
  }

  unique_ptr<IOBuf> head = requests.move();
  head->coalesce();
  transport_->addReadEvent(head->data(), head->length(),
                           std::chrono::milliseconds(0));
  transport_->startReadEvents();
  eventBase_.loop();

  NiceMock<MockHTTPCodecCallback> callbacks;

  std::list<HTTPCodec::StreamID> streams;
  EXPECT_CALL(callbacks, onMessageBegin(_, _))
    .Times(iterations * numPriorities);
  EXPECT_CALL(callbacks, onHeadersComplete(_, _))
    .Times(iterations * numPriorities);
  // body is variable and hence ignored
  EXPECT_CALL(callbacks, onMessageComplete(_, _))
    .Times(iterations * numPriorities)
    .WillRepeatedly(Invoke([&] (HTTPCodec::StreamID stream, bool upgrade) {
          streams.push_back(stream);
        }));

  clientCodec.setCallback(&callbacks);
  parseOutput(clientCodec);

  // transactions finish in priority order (higher streamIDs first)
  EXPECT_EQ(streams.size(), iterations * numPriorities);
  auto txn = streams.begin();
  for (int band = maxPriority; band >= 0; band--) {
    auto upperID = iterations * 2 * (band + 1);
    auto lowerID = iterations * 2 * band;
    for (auto i = 0; i < iterations; i++) {
      EXPECT_LE(lowerID, (uint32_t)*txn);
      EXPECT_GE(upperID, (uint32_t)*txn);
      ++txn;
    }
  }
}

// Verifies that the read timeout is not running when no ingress is expected/
// required to proceed
TEST_F(SPDY3DownstreamSessionTest, spdy_timeout) {
  IOBufQueue requests;
  HTTPMessage req = getGetRequest();
  SPDYCodec clientCodec(TransportDirection::UPSTREAM,
                        SPDYVersion::SPDY3);
  for (auto streamID = HTTPCodec::StreamID(1); streamID <= 3; streamID += 2) {
    clientCodec.generateHeader(requests, streamID, req);
    clientCodec.generateEOM(requests, streamID);
  }
  MockHTTPHandler* handler1 = new StrictMock<MockHTTPHandler>();
  MockHTTPHandler* handler2 = new StrictMock<MockHTTPHandler>();

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(handler1))
    .WillOnce(Return(handler2));

  InSequence handlerSequence;
  EXPECT_CALL(*handler1, setTransaction(_))
    .WillOnce(Invoke([handler1] (HTTPTransaction* txn) {
          handler1->txn_ = txn; }));
  EXPECT_CALL(*handler1, onHeadersComplete(_))
    .WillOnce(InvokeWithoutArgs([this] {   transport_->pauseWrites(); }));
  EXPECT_CALL(*handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([handler1] {
          handler1->sendHeaders(200, 1000);
          handler1->sendBody(100);
        }));
  EXPECT_CALL(*handler1, onEgressPaused());
  EXPECT_CALL(*handler2, setTransaction(_))
    .WillOnce(Invoke([handler2] (HTTPTransaction* txn) {
          handler2->txn_ = txn; }));
  EXPECT_CALL(*handler2, onEgressPaused());
  EXPECT_CALL(*handler2, onHeadersComplete(_));
  EXPECT_CALL(*handler2, onEOM())
    .WillOnce(InvokeWithoutArgs([handler2, this] {
          // This transaction should start egress paused.  We've received the
          // EOM, so the timeout shouldn't be running delay 400ms and resume
          // writes, this keeps txn1 from getting a write timeout
          eventBase_.runAfterDelay([this] {
              transport_->resumeWrites();
            }, 400);
        }));
  EXPECT_CALL(*handler1, onEgressResumed())
    .WillOnce(InvokeWithoutArgs([handler1] { handler1->txn_->sendEOM(); }));
  EXPECT_CALL(*handler2, onEgressResumed())
    .WillOnce(InvokeWithoutArgs([handler2, this] {
          // delay an additional 200ms.  The total 600ms delay shouldn't fire
          // onTimeout
          eventBase_.runAfterDelay([handler2] {
              handler2->sendReplyWithBody(200, 1000); }, 200
            );
        }));
  EXPECT_CALL(*handler1, detachTransaction())
    .WillOnce(InvokeWithoutArgs([handler1] { delete handler1; }));
  EXPECT_CALL(*handler2, detachTransaction())
    .WillOnce(InvokeWithoutArgs([handler2] { delete handler2; }));

  transport_->addReadEvent(requests, std::chrono::milliseconds(10));
  transport_->startReadEvents();
  eventBase_.loop();
}

// Verifies that the read timer is running while a transaction is blocked
// on a window update
TEST_F(SPDY3DownstreamSessionTest, spdy_timeout_win) {
  IOBufQueue requests;
  HTTPMessage req = getGetRequest();
  SPDYCodec clientCodec(TransportDirection::UPSTREAM,
                        SPDYVersion::SPDY3);
  auto streamID = HTTPCodec::StreamID(1);
  clientCodec.getEgressSettings()->setSetting(SettingsId::INITIAL_WINDOW_SIZE,
                                              500);
  clientCodec.generateSettings(requests);
  clientCodec.generateHeader(requests, streamID, req, 0, nullptr);
  clientCodec.generateEOM(requests, streamID);
  StrictMock<MockHTTPHandler> handler;

  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler));

  InSequence handlerSequence;
  EXPECT_CALL(handler, setTransaction(_))
    .WillOnce(Invoke([&] (HTTPTransaction* txn) {
          handler.txn_ = txn; }));
  EXPECT_CALL(handler, onHeadersComplete(_));
  EXPECT_CALL(handler, onEOM())
    .WillOnce(InvokeWithoutArgs([&] {
          handler.sendReplyWithBody(200, 1000);
        }));
  EXPECT_CALL(handler, onEgressPaused());
  EXPECT_CALL(handler, onError(_))
    .WillOnce(Invoke([&] (const HTTPException& ex) {
          ASSERT_EQ(ex.getProxygenError(), kErrorTimeout);
          handler.terminate();
        }));
  EXPECT_CALL(handler, detachTransaction());

  transport_->addReadEvent(requests, std::chrono::milliseconds(10));
  transport_->startReadEvents();
  eventBase_.loop();
}

TYPED_TEST_CASE_P(HTTPDownstreamTest);

TYPED_TEST_P(HTTPDownstreamTest, testWritesDraining) {
  IOBufQueue requests;
  HTTPMessage req = getGetRequest();
  auto clientCodec =
    makeClientCodec<typename TypeParam::Codec>(TypeParam::version);
  auto badCodec =
    makeServerCodec<typename TypeParam::Codec>(TypeParam::version);
  auto streamID = HTTPCodec::StreamID(1);
  clientCodec->generateHeader(requests, streamID, req);
  clientCodec->generateEOM(requests, streamID);
  streamID += 1;
  badCodec->generateHeader(requests, streamID, req, 1);
  MockHTTPHandler handler1;

  EXPECT_CALL(this->mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1));
  EXPECT_CALL(this->mockController_, detachSession(_));

  InSequence handlerSequence;
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM());
  EXPECT_CALL(handler1, onError(_));
  EXPECT_CALL(handler1, detachTransaction());

  this->transport_->addReadEvent(requests, std::chrono::milliseconds(10));
  this->transport_->startReadEvents();
  this->eventBase_.loop();
}

// Set max streams=1
// send two spdy requests a few ms apart.
// Block writes
// generate a complete response for txn=1 before parsing txn=3
// HTTPSession should allow the txn=3 to be served rather than refusing it
TEST_F(SPDY3DownstreamSessionTest, spdy_max_concurrent_streams) {
  IOBufQueue requests;
  StrictMock<MockHTTPHandler> handler1;
  StrictMock<MockHTTPHandler> handler2;
  HTTPMessage req = getGetRequest();
  req.setHTTPVersion(1, 0);
  req.setWantsKeepalive(false);
  SPDYCodec clientCodec(TransportDirection::UPSTREAM,
                        SPDYVersion::SPDY3);
  auto streamID = HTTPCodec::StreamID(1);
  clientCodec.generateHeader(requests, streamID, req);
  clientCodec.generateEOM(requests, streamID);
  streamID += 2;
  clientCodec.generateHeader(requests, streamID, req);
  clientCodec.generateEOM(requests, streamID);

  httpSession_->getCodecFilterChain()->getEgressSettings()->setSetting(
    SettingsId::MAX_CONCURRENT_STREAMS, 1);
  EXPECT_CALL(mockController_, getRequestHandler(_, _))
    .WillOnce(Return(&handler1))
    .WillOnce(Return(&handler2));

  InSequence handlerSequence;
  EXPECT_CALL(handler1, setTransaction(_))
    .WillOnce(Invoke([&handler1] (HTTPTransaction* txn) {
          handler1.txn_ = txn; }));
  EXPECT_CALL(handler1, onHeadersComplete(_));
  EXPECT_CALL(handler1, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler1, this] {
          transport_->pauseWrites();
          handler1.sendReplyWithBody(200, 100);
        }));
  EXPECT_CALL(handler1, onEgressPaused());
  EXPECT_CALL(handler2, setTransaction(_))
    .WillOnce(Invoke([&handler2] (HTTPTransaction* txn) {
          handler2.txn_ = txn; }));
  EXPECT_CALL(handler2, onEgressPaused());
  EXPECT_CALL(handler2, onHeadersComplete(_));
  EXPECT_CALL(handler2, onEOM())
    .WillOnce(InvokeWithoutArgs([&handler2, this] {
          handler2.sendReplyWithBody(200, 100);
          eventBase_.runInLoop([this] {
              transport_->resumeWrites();
            });
        }));
  EXPECT_CALL(handler1, detachTransaction());
  EXPECT_CALL(handler2, onEgressResumed());
  EXPECT_CALL(handler2, detachTransaction());

  EXPECT_CALL(mockController_, detachSession(_));

  transport_->addReadEvent(requests, std::chrono::milliseconds(10));
  transport_->startReadEvents();
  transport_->addReadEOF(std::chrono::milliseconds(10));

  eventBase_.loop();
}

REGISTER_TYPED_TEST_CASE_P(HTTPDownstreamTest,
                           testWritesDraining);

typedef ::testing::Types<SPDY2CodecPair, SPDY3CodecPair> ParallelCodecs;
INSTANTIATE_TYPED_TEST_CASE_P(ParallelCodecs,
                              HTTPDownstreamTest,
                              ParallelCodecs);

class SPDY31DownstreamTest : public HTTPDownstreamTest<SPDY3_1CodecPair> {
 public:
  SPDY31DownstreamTest()
      : HTTPDownstreamTest<SPDY3_1CodecPair>(2 * spdy::kInitialWindow) {}
};

TEST_F(SPDY31DownstreamTest, testSessionFlowControl) {
  eventBase_.loopOnce();
  NiceMock<MockHTTPCodecCallback> callbacks;
  SPDYCodec clientCodec(TransportDirection::UPSTREAM,
                        SPDYVersion::SPDY3_1);

  EXPECT_CALL(callbacks, onWindowUpdate(0, spdy::kInitialWindow));
  clientCodec.setCallback(&callbacks);
  parseOutput(clientCodec);
}
