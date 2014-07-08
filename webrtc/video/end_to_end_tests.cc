/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include <assert.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

#include "webrtc/call.h"
#include "webrtc/frame_callback.h"
#include "webrtc/modules/rtp_rtcp/source/rtcp_utility.h"
#include "webrtc/modules/video_coding/codecs/vp8/include/vp8.h"
#include "webrtc/system_wrappers/interface/critical_section_wrapper.h"
#include "webrtc/system_wrappers/interface/event_wrapper.h"
#include "webrtc/system_wrappers/interface/scoped_ptr.h"
#include "webrtc/system_wrappers/interface/sleep.h"
#include "webrtc/test/call_test.h"
#include "webrtc/test/direct_transport.h"
#include "webrtc/test/encoder_settings.h"
#include "webrtc/test/fake_audio_device.h"
#include "webrtc/test/fake_decoder.h"
#include "webrtc/test/fake_encoder.h"
#include "webrtc/test/frame_generator.h"
#include "webrtc/test/frame_generator_capturer.h"
#include "webrtc/test/null_transport.h"
#include "webrtc/test/rtp_rtcp_observer.h"
#include "webrtc/test/testsupport/fileutils.h"
#include "webrtc/test/testsupport/perf_test.h"
#include "webrtc/video/transport_adapter.h"

namespace webrtc {

static const int kRedPayloadType = 118;
static const int kUlpfecPayloadType = 119;

class EndToEndTest : public test::CallTest {
 public:
  EndToEndTest() {}

  virtual ~EndToEndTest() {
    EXPECT_EQ(NULL, send_stream_);
    EXPECT_TRUE(receive_streams_.empty());
  }

 protected:
  void DecodesRetransmittedFrame(bool retransmit_over_rtx);
  void ReceivesPliAndRecovers(int rtp_history_ms);
  void RespectsRtcpMode(newapi::RtcpMode rtcp_mode);
  void TestXrReceiverReferenceTimeReport(bool enable_rrtr);
  void TestSendsSetSsrcs(size_t num_ssrcs, bool send_single_ssrc_first);
  void TestRtpStatePreservation(bool use_rtx);
};

TEST_F(EndToEndTest, ReceiverCanBeStartedTwice) {
  test::NullTransport transport;
  CreateCalls(Call::Config(&transport), Call::Config(&transport));

  CreateSendConfig(1);
  CreateMatchingReceiveConfigs();

  CreateStreams();

  receive_streams_[0]->Start();
  receive_streams_[0]->Start();

  DestroyStreams();
}

TEST_F(EndToEndTest, ReceiverCanBeStoppedTwice) {
  test::NullTransport transport;
  CreateCalls(Call::Config(&transport), Call::Config(&transport));

  CreateSendConfig(1);
  CreateMatchingReceiveConfigs();

  CreateStreams();

  receive_streams_[0]->Stop();
  receive_streams_[0]->Stop();

  DestroyStreams();
}

TEST_F(EndToEndTest, RendersSingleDelayedFrame) {
  static const int kWidth = 320;
  static const int kHeight = 240;
  // This constant is chosen to be higher than the timeout in the video_render
  // module. This makes sure that frames aren't dropped if there are no other
  // frames in the queue.
  static const int kDelayRenderCallbackMs = 1000;

  class Renderer : public VideoRenderer {
   public:
    Renderer() : event_(EventWrapper::Create()) {}

    virtual void RenderFrame(const I420VideoFrame& video_frame,
                             int /*time_to_render_ms*/) OVERRIDE {
      event_->Set();
    }

    EventTypeWrapper Wait() { return event_->Wait(kDefaultTimeoutMs); }

    scoped_ptr<EventWrapper> event_;
  } renderer;

  class TestFrameCallback : public I420FrameCallback {
   public:
    TestFrameCallback() : event_(EventWrapper::Create()) {}

    EventTypeWrapper Wait() { return event_->Wait(kDefaultTimeoutMs); }

   private:
    virtual void FrameCallback(I420VideoFrame* frame) OVERRIDE {
      SleepMs(kDelayRenderCallbackMs);
      event_->Set();
    }

    scoped_ptr<EventWrapper> event_;
  };

  test::DirectTransport sender_transport, receiver_transport;

  CreateCalls(Call::Config(&sender_transport),
              Call::Config(&receiver_transport));

  sender_transport.SetReceiver(receiver_call_->Receiver());
  receiver_transport.SetReceiver(sender_call_->Receiver());

  CreateSendConfig(1);
  CreateMatchingReceiveConfigs();

  TestFrameCallback pre_render_callback;
  receive_configs_[0].pre_render_callback = &pre_render_callback;
  receive_configs_[0].renderer = &renderer;

  CreateStreams();
  Start();

  // Create frames that are smaller than the send width/height, this is done to
  // check that the callbacks are done after processing video.
  scoped_ptr<test::FrameGenerator> frame_generator(
      test::FrameGenerator::Create(kWidth, kHeight));
  send_stream_->Input()->SwapFrame(frame_generator->NextFrame());
  EXPECT_EQ(kEventSignaled, pre_render_callback.Wait())
      << "Timed out while waiting for pre-render callback.";
  EXPECT_EQ(kEventSignaled, renderer.Wait())
      << "Timed out while waiting for the frame to render.";

  Stop();

  sender_transport.StopSending();
  receiver_transport.StopSending();

  DestroyStreams();
}

TEST_F(EndToEndTest, TransmitsFirstFrame) {
  class Renderer : public VideoRenderer {
   public:
    Renderer() : event_(EventWrapper::Create()) {}

    virtual void RenderFrame(const I420VideoFrame& video_frame,
                             int /*time_to_render_ms*/) OVERRIDE {
      event_->Set();
    }

    EventTypeWrapper Wait() { return event_->Wait(kDefaultTimeoutMs); }

    scoped_ptr<EventWrapper> event_;
  } renderer;

  test::DirectTransport sender_transport, receiver_transport;

  CreateCalls(Call::Config(&sender_transport),
              Call::Config(&receiver_transport));

  sender_transport.SetReceiver(receiver_call_->Receiver());
  receiver_transport.SetReceiver(sender_call_->Receiver());

  CreateSendConfig(1);
  CreateMatchingReceiveConfigs();
  receive_configs_[0].renderer = &renderer;

  CreateStreams();
  Start();

  scoped_ptr<test::FrameGenerator> frame_generator(test::FrameGenerator::Create(
      video_streams_[0].width, video_streams_[0].height));
  send_stream_->Input()->SwapFrame(frame_generator->NextFrame());

  EXPECT_EQ(kEventSignaled, renderer.Wait())
      << "Timed out while waiting for the frame to render.";

  Stop();

  sender_transport.StopSending();
  receiver_transport.StopSending();

  DestroyStreams();
}

TEST_F(EndToEndTest, ReceiverUsesLocalSsrc) {
  class SyncRtcpObserver : public test::EndToEndTest {
   public:
    SyncRtcpObserver() : EndToEndTest(kDefaultTimeoutMs) {}

    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());
      uint32_t ssrc = 0;
      ssrc |= static_cast<uint32_t>(packet[4]) << 24;
      ssrc |= static_cast<uint32_t>(packet[5]) << 16;
      ssrc |= static_cast<uint32_t>(packet[6]) << 8;
      ssrc |= static_cast<uint32_t>(packet[7]) << 0;
      EXPECT_EQ(kReceiverLocalSsrc, ssrc);
      observation_complete_->Set();

      return SEND_PACKET;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while waiting for a receiver RTCP packet to be sent.";
    }
  } test;

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, ReceivesAndRetransmitsNack) {
  static const int kNumberOfNacksToObserve = 2;
  static const int kLossBurstSize = 2;
  static const int kPacketsBetweenLossBursts = 9;
  class NackObserver : public test::EndToEndTest {
   public:
    NackObserver()
        : EndToEndTest(kLongTimeoutMs),
          rtp_parser_(RtpHeaderParser::Create()),
          sent_rtp_packets_(0),
          packets_left_to_drop_(0),
          nacks_left_(kNumberOfNacksToObserve) {}

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      RTPHeader header;
      EXPECT_TRUE(rtp_parser_->Parse(packet, length, &header));

      // Never drop retransmitted packets.
      if (dropped_packets_.find(header.sequenceNumber) !=
          dropped_packets_.end()) {
        retransmitted_packets_.insert(header.sequenceNumber);
        if (nacks_left_ == 0 &&
            retransmitted_packets_.size() == dropped_packets_.size()) {
          observation_complete_->Set();
        }
        return SEND_PACKET;
      }

      ++sent_rtp_packets_;

      // Enough NACKs received, stop dropping packets.
      if (nacks_left_ == 0)
        return SEND_PACKET;

      // Check if it's time for a new loss burst.
      if (sent_rtp_packets_ % kPacketsBetweenLossBursts == 0)
        packets_left_to_drop_ = kLossBurstSize;

      if (packets_left_to_drop_ > 0) {
        --packets_left_to_drop_;
        dropped_packets_.insert(header.sequenceNumber);
        return DROP_PACKET;
      }

      return SEND_PACKET;
    }

    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());

      RTCPUtility::RTCPPacketTypes packet_type = parser.Begin();
      while (packet_type != RTCPUtility::kRtcpNotValidCode) {
        if (packet_type == RTCPUtility::kRtcpRtpfbNackCode) {
          --nacks_left_;
          break;
        }
        packet_type = parser.Iterate();
      }
      return SEND_PACKET;
    }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      send_config->rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
      (*receive_configs)[0].rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out waiting for packets to be NACKed, retransmitted and "
             "rendered.";
    }

    scoped_ptr<RtpHeaderParser> rtp_parser_;
    std::set<uint16_t> dropped_packets_;
    std::set<uint16_t> retransmitted_packets_;
    uint64_t sent_rtp_packets_;
    int packets_left_to_drop_;
    int nacks_left_;
  } test;

  RunBaseTest(&test);
}

// TODO(pbos): Flaky, webrtc:3269
TEST_F(EndToEndTest, DISABLED_CanReceiveFec) {
  class FecRenderObserver : public test::EndToEndTest, public VideoRenderer {
   public:
    FecRenderObserver()
        : EndToEndTest(kDefaultTimeoutMs),
          state_(kFirstPacket),
          protected_sequence_number_(0),
          protected_frame_timestamp_(0) {}

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE
        EXCLUSIVE_LOCKS_REQUIRED(crit_) {
      RTPHeader header;
      EXPECT_TRUE(parser_->Parse(packet, length, &header));

      EXPECT_EQ(kRedPayloadType, header.payloadType);
      int encapsulated_payload_type =
          static_cast<int>(packet[header.headerLength]);
      if (encapsulated_payload_type != kFakeSendPayloadType)
        EXPECT_EQ(kUlpfecPayloadType, encapsulated_payload_type);

      switch (state_) {
        case kFirstPacket:
          state_ = kDropEveryOtherPacketUntilFec;
          break;
        case kDropEveryOtherPacketUntilFec:
          if (encapsulated_payload_type == kUlpfecPayloadType) {
            state_ = kDropNextMediaPacket;
            return SEND_PACKET;
          }
          if (header.sequenceNumber % 2 == 0)
            return DROP_PACKET;
          break;
        case kDropNextMediaPacket:
          if (encapsulated_payload_type == kFakeSendPayloadType) {
            protected_sequence_number_ = header.sequenceNumber;
            protected_frame_timestamp_ = header.timestamp;
            state_ = kProtectedPacketDropped;
            return DROP_PACKET;
          }
          break;
        case kProtectedPacketDropped:
          EXPECT_NE(header.sequenceNumber, protected_sequence_number_)
              << "Protected packet retransmitted. Should not happen with FEC.";
          break;
      }

      return SEND_PACKET;
    }

    virtual void RenderFrame(const I420VideoFrame& video_frame,
                             int time_to_render_ms) OVERRIDE {
      CriticalSectionScoped lock(crit_.get());
      // Rendering frame with timestamp associated with dropped packet -> FEC
      // protection worked.
      if (state_ == kProtectedPacketDropped &&
          video_frame.timestamp() == protected_frame_timestamp_) {
        observation_complete_->Set();
      }
    }

    enum {
      kFirstPacket,
      kDropEveryOtherPacketUntilFec,
      kDropNextMediaPacket,
      kProtectedPacketDropped,
    } state_;

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      // TODO(pbos): Run this test with combined NACK/FEC enabled as well.
      // int rtp_history_ms = 1000;
      // (*receive_configs)[0].rtp.nack.rtp_history_ms = rtp_history_ms;
      // send_config->rtp.nack.rtp_history_ms = rtp_history_ms;
      send_config->rtp.fec.red_payload_type = kRedPayloadType;
      send_config->rtp.fec.ulpfec_payload_type = kUlpfecPayloadType;

      (*receive_configs)[0].rtp.fec.red_payload_type = kRedPayloadType;
      (*receive_configs)[0].rtp.fec.ulpfec_payload_type = kUlpfecPayloadType;
      (*receive_configs)[0].renderer = this;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while waiting for retransmitted NACKed frames to be "
             "rendered again.";
    }

    uint32_t protected_sequence_number_ GUARDED_BY(crit_);
    uint32_t protected_frame_timestamp_ GUARDED_BY(crit_);
  } test;

  RunBaseTest(&test);
}

// This test drops second RTP packet with a marker bit set, makes sure it's
// retransmitted and renders. Retransmission SSRCs are also checked.
void EndToEndTest::DecodesRetransmittedFrame(bool retransmit_over_rtx) {
  static const int kDroppedFrameNumber = 2;
  class RetransmissionObserver : public test::EndToEndTest,
                                 public I420FrameCallback {
   public:
    explicit RetransmissionObserver(bool expect_rtx)
        : EndToEndTest(kDefaultTimeoutMs),
          retransmission_ssrc_(expect_rtx ? kSendRtxSsrcs[0] : kSendSsrcs[0]),
          retransmission_payload_type_(expect_rtx ? kSendRtxPayloadType
                                                  : kFakeSendPayloadType),
          marker_bits_observed_(0),
          retransmitted_timestamp_(0),
          frame_retransmitted_(false) {}

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      RTPHeader header;
      EXPECT_TRUE(parser_->Parse(packet, length, &header));

      if (header.timestamp == retransmitted_timestamp_) {
        EXPECT_EQ(retransmission_ssrc_, header.ssrc);
        EXPECT_EQ(retransmission_payload_type_, header.payloadType);
        frame_retransmitted_ = true;
        return SEND_PACKET;
      }

      EXPECT_EQ(kSendSsrcs[0], header.ssrc);
      EXPECT_EQ(kFakeSendPayloadType, header.payloadType);

      // Found the second frame's final packet, drop this and expect a
      // retransmission.
      if (header.markerBit && ++marker_bits_observed_ == kDroppedFrameNumber) {
        retransmitted_timestamp_ = header.timestamp;
        return DROP_PACKET;
      }

      return SEND_PACKET;
    }

    virtual void FrameCallback(I420VideoFrame* frame) OVERRIDE {
      CriticalSectionScoped lock(crit_.get());
      if (frame->timestamp() == retransmitted_timestamp_) {
        EXPECT_TRUE(frame_retransmitted_);
        observation_complete_->Set();
      }
    }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      send_config->rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
      (*receive_configs)[0].pre_render_callback = this;
      (*receive_configs)[0].rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
      if (retransmission_ssrc_ == kSendRtxSsrcs[0]) {
        send_config->rtp.rtx.ssrcs.push_back(kSendRtxSsrcs[0]);
        send_config->rtp.rtx.payload_type = kSendRtxPayloadType;
        (*receive_configs)[0].rtp.rtx[kSendRtxPayloadType].ssrc =
            kSendRtxSsrcs[0];
        (*receive_configs)[0].rtp.rtx[kSendRtxPayloadType].payload_type =
            kSendRtxPayloadType;
      }
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while waiting for retransmission to render.";
    }

    const uint32_t retransmission_ssrc_;
    const int retransmission_payload_type_;
    int marker_bits_observed_;
    uint32_t retransmitted_timestamp_;
    bool frame_retransmitted_;
  } test(retransmit_over_rtx);

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, DecodesRetransmittedFrame) {
  DecodesRetransmittedFrame(false);
}

TEST_F(EndToEndTest, DecodesRetransmittedFrameOverRtx) {
  DecodesRetransmittedFrame(true);
}

TEST_F(EndToEndTest, UsesFrameCallbacks) {
  static const int kWidth = 320;
  static const int kHeight = 240;

  class Renderer : public VideoRenderer {
   public:
    Renderer() : event_(EventWrapper::Create()) {}

    virtual void RenderFrame(const I420VideoFrame& video_frame,
                             int /*time_to_render_ms*/) OVERRIDE {
      EXPECT_EQ(0, *video_frame.buffer(kYPlane))
          << "Rendered frame should have zero luma which is applied by the "
             "pre-render callback.";
      event_->Set();
    }

    EventTypeWrapper Wait() { return event_->Wait(kDefaultTimeoutMs); }
    scoped_ptr<EventWrapper> event_;
  } renderer;

  class TestFrameCallback : public I420FrameCallback {
   public:
    TestFrameCallback(int expected_luma_byte, int next_luma_byte)
        : event_(EventWrapper::Create()),
          expected_luma_byte_(expected_luma_byte),
          next_luma_byte_(next_luma_byte) {}

    EventTypeWrapper Wait() { return event_->Wait(kDefaultTimeoutMs); }

   private:
    virtual void FrameCallback(I420VideoFrame* frame) {
      EXPECT_EQ(kWidth, frame->width())
          << "Width not as expected, callback done before resize?";
      EXPECT_EQ(kHeight, frame->height())
          << "Height not as expected, callback done before resize?";

      // Previous luma specified, observed luma should be fairly close.
      if (expected_luma_byte_ != -1) {
        EXPECT_NEAR(expected_luma_byte_, *frame->buffer(kYPlane), 10);
      }

      memset(frame->buffer(kYPlane),
             next_luma_byte_,
             frame->allocated_size(kYPlane));

      event_->Set();
    }

    scoped_ptr<EventWrapper> event_;
    int expected_luma_byte_;
    int next_luma_byte_;
  };

  TestFrameCallback pre_encode_callback(-1, 255);  // Changes luma to 255.
  TestFrameCallback pre_render_callback(255, 0);  // Changes luma from 255 to 0.

  test::DirectTransport sender_transport, receiver_transport;

  CreateCalls(Call::Config(&sender_transport),
              Call::Config(&receiver_transport));

  sender_transport.SetReceiver(receiver_call_->Receiver());
  receiver_transport.SetReceiver(sender_call_->Receiver());

  CreateSendConfig(1);
  scoped_ptr<VP8Encoder> encoder(VP8Encoder::Create());
  send_config_.encoder_settings.encoder = encoder.get();
  send_config_.encoder_settings.payload_name = "VP8";
  ASSERT_EQ(1u, video_streams_.size()) << "Test setup error.";
  video_streams_[0].width = kWidth;
  video_streams_[0].height = kHeight;
  send_config_.pre_encode_callback = &pre_encode_callback;

  CreateMatchingReceiveConfigs();
  receive_configs_[0].pre_render_callback = &pre_render_callback;
  receive_configs_[0].renderer = &renderer;

  CreateStreams();
  Start();

  // Create frames that are smaller than the send width/height, this is done to
  // check that the callbacks are done after processing video.
  scoped_ptr<test::FrameGenerator> frame_generator(
      test::FrameGenerator::Create(kWidth / 2, kHeight / 2));
  send_stream_->Input()->SwapFrame(frame_generator->NextFrame());

  EXPECT_EQ(kEventSignaled, pre_encode_callback.Wait())
      << "Timed out while waiting for pre-encode callback.";
  EXPECT_EQ(kEventSignaled, pre_render_callback.Wait())
      << "Timed out while waiting for pre-render callback.";
  EXPECT_EQ(kEventSignaled, renderer.Wait())
      << "Timed out while waiting for the frame to render.";

  Stop();

  sender_transport.StopSending();
  receiver_transport.StopSending();

  DestroyStreams();
}

void EndToEndTest::ReceivesPliAndRecovers(int rtp_history_ms) {
  static const int kPacketsToDrop = 1;

  class PliObserver : public test::EndToEndTest, public VideoRenderer {
   public:
    explicit PliObserver(int rtp_history_ms)
        : EndToEndTest(kLongTimeoutMs),
          rtp_history_ms_(rtp_history_ms),
          nack_enabled_(rtp_history_ms > 0),
          highest_dropped_timestamp_(0),
          frames_to_drop_(0),
          received_pli_(false) {}

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      RTPHeader header;
      EXPECT_TRUE(parser_->Parse(packet, length, &header));

      // Drop all retransmitted packets to force a PLI.
      if (header.timestamp <= highest_dropped_timestamp_)
        return DROP_PACKET;

      if (frames_to_drop_ > 0) {
        highest_dropped_timestamp_ = header.timestamp;
        --frames_to_drop_;
        return DROP_PACKET;
      }

      return SEND_PACKET;
    }

    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());

      for (RTCPUtility::RTCPPacketTypes packet_type = parser.Begin();
           packet_type != RTCPUtility::kRtcpNotValidCode;
           packet_type = parser.Iterate()) {
        if (!nack_enabled_)
          EXPECT_NE(packet_type, RTCPUtility::kRtcpRtpfbNackCode);

        if (packet_type == RTCPUtility::kRtcpPsfbPliCode) {
          received_pli_ = true;
          break;
        }
      }
      return SEND_PACKET;
    }

    virtual void RenderFrame(const I420VideoFrame& video_frame,
                             int time_to_render_ms) OVERRIDE {
      CriticalSectionScoped lock(crit_.get());
      if (received_pli_ &&
          video_frame.timestamp() > highest_dropped_timestamp_) {
        observation_complete_->Set();
      }
      if (!received_pli_)
        frames_to_drop_ = kPacketsToDrop;
    }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      send_config->rtp.nack.rtp_history_ms = rtp_history_ms_;
      (*receive_configs)[0].rtp.nack.rtp_history_ms = rtp_history_ms_;
      (*receive_configs)[0].renderer = this;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait()) << "Timed out waiting for PLI to be "
                                           "received and a frame to be "
                                           "rendered afterwards.";
    }

    int rtp_history_ms_;
    bool nack_enabled_;
    uint32_t highest_dropped_timestamp_;
    int frames_to_drop_;
    bool received_pli_;
  } test(rtp_history_ms);

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, ReceivesPliAndRecoversWithNack) {
  ReceivesPliAndRecovers(1000);
}

// TODO(pbos): Enable this when 2250 is resolved.
TEST_F(EndToEndTest, DISABLED_ReceivesPliAndRecoversWithoutNack) {
  ReceivesPliAndRecovers(0);
}

TEST_F(EndToEndTest, UnknownRtpPacketGivesUnknownSsrcReturnCode) {
  class PacketInputObserver : public PacketReceiver {
   public:
    explicit PacketInputObserver(PacketReceiver* receiver)
        : receiver_(receiver), delivered_packet_(EventWrapper::Create()) {}

    EventTypeWrapper Wait() {
      return delivered_packet_->Wait(kDefaultTimeoutMs);
    }

   private:
    virtual DeliveryStatus DeliverPacket(const uint8_t* packet,
                                         size_t length) OVERRIDE {
      if (RtpHeaderParser::IsRtcp(packet, length)) {
        return receiver_->DeliverPacket(packet, length);
      } else {
        DeliveryStatus delivery_status =
            receiver_->DeliverPacket(packet, length);
        EXPECT_EQ(DELIVERY_UNKNOWN_SSRC, delivery_status);
        delivered_packet_->Set();
        return delivery_status;
      }
    }

    PacketReceiver* receiver_;
    scoped_ptr<EventWrapper> delivered_packet_;
  };

  test::DirectTransport send_transport, receive_transport;

  CreateCalls(Call::Config(&send_transport), Call::Config(&receive_transport));
  PacketInputObserver input_observer(receiver_call_->Receiver());

  send_transport.SetReceiver(&input_observer);
  receive_transport.SetReceiver(sender_call_->Receiver());

  CreateSendConfig(1);
  CreateMatchingReceiveConfigs();

  CreateStreams();
  CreateFrameGeneratorCapturer();
  Start();

  receiver_call_->DestroyVideoReceiveStream(receive_streams_[0]);
  receive_streams_.clear();

  // Wait() waits for a received packet.
  EXPECT_EQ(kEventSignaled, input_observer.Wait());

  Stop();

  DestroyStreams();

  send_transport.StopSending();
  receive_transport.StopSending();
}

void EndToEndTest::RespectsRtcpMode(newapi::RtcpMode rtcp_mode) {
  static const int kNumCompoundRtcpPacketsToObserve = 10;
  class RtcpModeObserver : public test::EndToEndTest {
   public:
    explicit RtcpModeObserver(newapi::RtcpMode rtcp_mode)
        : EndToEndTest(kDefaultTimeoutMs),
          rtcp_mode_(rtcp_mode),
          sent_rtp_(0),
          sent_rtcp_(0) {}

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      if (++sent_rtp_ % 3 == 0)
        return DROP_PACKET;

      return SEND_PACKET;
    }

    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      ++sent_rtcp_;
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());

      RTCPUtility::RTCPPacketTypes packet_type = parser.Begin();
      bool has_report_block = false;
      while (packet_type != RTCPUtility::kRtcpNotValidCode) {
        EXPECT_NE(RTCPUtility::kRtcpSrCode, packet_type);
        if (packet_type == RTCPUtility::kRtcpRrCode) {
          has_report_block = true;
          break;
        }
        packet_type = parser.Iterate();
      }

      switch (rtcp_mode_) {
        case newapi::kRtcpCompound:
          if (!has_report_block) {
            ADD_FAILURE() << "Received RTCP packet without receiver report for "
                             "kRtcpCompound.";
            observation_complete_->Set();
          }

          if (sent_rtcp_ >= kNumCompoundRtcpPacketsToObserve)
            observation_complete_->Set();

          break;
        case newapi::kRtcpReducedSize:
          if (!has_report_block)
            observation_complete_->Set();
          break;
      }

      return SEND_PACKET;
    }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      send_config->rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
      (*receive_configs)[0].rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
      (*receive_configs)[0].rtp.rtcp_mode = rtcp_mode_;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << (rtcp_mode_ == newapi::kRtcpCompound
                  ? "Timed out before observing enough compound packets."
                  : "Timed out before receiving a non-compound RTCP packet.");
    }

    newapi::RtcpMode rtcp_mode_;
    int sent_rtp_;
    int sent_rtcp_;
  } test(rtcp_mode);

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, UsesRtcpCompoundMode) {
  RespectsRtcpMode(newapi::kRtcpCompound);
}

TEST_F(EndToEndTest, UsesRtcpReducedSizeMode) {
  RespectsRtcpMode(newapi::kRtcpReducedSize);
}

// Test sets up a Call multiple senders with different resolutions and SSRCs.
// Another is set up to receive all three of these with different renderers.
// Each renderer verifies that it receives the expected resolution, and as soon
// as every renderer has received a frame, the test finishes.
TEST_F(EndToEndTest, SendsAndReceivesMultipleStreams) {
  static const size_t kNumStreams = 3;

  class VideoOutputObserver : public VideoRenderer {
   public:
    VideoOutputObserver(test::FrameGeneratorCapturer** capturer,
                        int width,
                        int height)
        : capturer_(capturer),
          width_(width),
          height_(height),
          done_(EventWrapper::Create()) {}

    virtual void RenderFrame(const I420VideoFrame& video_frame,
                             int time_to_render_ms) OVERRIDE {
      EXPECT_EQ(width_, video_frame.width());
      EXPECT_EQ(height_, video_frame.height());
      (*capturer_)->Stop();
      done_->Set();
    }

    EventTypeWrapper Wait() { return done_->Wait(kDefaultTimeoutMs); }

   private:
    test::FrameGeneratorCapturer** capturer_;
    int width_;
    int height_;
    scoped_ptr<EventWrapper> done_;
  };

  struct {
    uint32_t ssrc;
    int width;
    int height;
  } codec_settings[kNumStreams] = {{1, 640, 480}, {2, 320, 240}, {3, 240, 160}};

  test::DirectTransport sender_transport, receiver_transport;
  scoped_ptr<Call> sender_call(Call::Create(Call::Config(&sender_transport)));
  scoped_ptr<Call> receiver_call(
      Call::Create(Call::Config(&receiver_transport)));
  sender_transport.SetReceiver(receiver_call->Receiver());
  receiver_transport.SetReceiver(sender_call->Receiver());

  VideoSendStream* send_streams[kNumStreams];
  VideoReceiveStream* receive_streams[kNumStreams];

  VideoOutputObserver* observers[kNumStreams];
  test::FrameGeneratorCapturer* frame_generators[kNumStreams];

  scoped_ptr<VP8Encoder> encoders[kNumStreams];
  for (size_t i = 0; i < kNumStreams; ++i)
    encoders[i].reset(VP8Encoder::Create());

  for (size_t i = 0; i < kNumStreams; ++i) {
    uint32_t ssrc = codec_settings[i].ssrc;
    int width = codec_settings[i].width;
    int height = codec_settings[i].height;
    observers[i] = new VideoOutputObserver(&frame_generators[i], width, height);

    VideoSendStream::Config send_config;
    send_config.rtp.ssrcs.push_back(ssrc);
    send_config.encoder_settings.encoder = encoders[i].get();
    send_config.encoder_settings.payload_name = "VP8";
    send_config.encoder_settings.payload_type = 124;
    std::vector<VideoStream> video_streams = test::CreateVideoStreams(1);
    VideoStream* stream = &video_streams[0];
    stream->width = width;
    stream->height = height;
    stream->max_framerate = 5;
    stream->min_bitrate_bps = stream->target_bitrate_bps =
        stream->max_bitrate_bps = 100000;
    send_streams[i] =
        sender_call->CreateVideoSendStream(send_config, video_streams, NULL);
    send_streams[i]->Start();

    VideoReceiveStream::Config receive_config;
    receive_config.renderer = observers[i];
    receive_config.rtp.remote_ssrc = ssrc;
    receive_config.rtp.local_ssrc = kReceiverLocalSsrc;
    VideoCodec codec =
        test::CreateDecoderVideoCodec(send_config.encoder_settings);
    receive_config.codecs.push_back(codec);
    receive_streams[i] =
        receiver_call->CreateVideoReceiveStream(receive_config);
    receive_streams[i]->Start();

    frame_generators[i] = test::FrameGeneratorCapturer::Create(
        send_streams[i]->Input(), width, height, 30, Clock::GetRealTimeClock());
    frame_generators[i]->Start();
  }

  for (size_t i = 0; i < kNumStreams; ++i) {
    EXPECT_EQ(kEventSignaled, observers[i]->Wait())
        << "Timed out while waiting for observer " << i << " to render.";
  }

  for (size_t i = 0; i < kNumStreams; ++i) {
    frame_generators[i]->Stop();
    sender_call->DestroyVideoSendStream(send_streams[i]);
    receiver_call->DestroyVideoReceiveStream(receive_streams[i]);
    delete frame_generators[i];
    delete observers[i];
  }

  sender_transport.StopSending();
  receiver_transport.StopSending();
}

TEST_F(EndToEndTest, ObserversEncodedFrames) {
  class EncodedFrameTestObserver : public EncodedFrameObserver {
   public:
    EncodedFrameTestObserver()
        : length_(0),
          frame_type_(kFrameEmpty),
          called_(EventWrapper::Create()) {}
    virtual ~EncodedFrameTestObserver() {}

    virtual void EncodedFrameCallback(const EncodedFrame& encoded_frame) {
      frame_type_ = encoded_frame.frame_type_;
      length_ = encoded_frame.length_;
      buffer_.reset(new uint8_t[length_]);
      memcpy(buffer_.get(), encoded_frame.data_, length_);
      called_->Set();
    }

    EventTypeWrapper Wait() { return called_->Wait(kDefaultTimeoutMs); }

    void ExpectEqualFrames(const EncodedFrameTestObserver& observer) {
      ASSERT_EQ(length_, observer.length_)
          << "Observed frames are of different lengths.";
      EXPECT_EQ(frame_type_, observer.frame_type_)
          << "Observed frames have different frame types.";
      EXPECT_EQ(0, memcmp(buffer_.get(), observer.buffer_.get(), length_))
          << "Observed encoded frames have different content.";
    }

   private:
    scoped_ptr<uint8_t[]> buffer_;
    size_t length_;
    FrameType frame_type_;
    scoped_ptr<EventWrapper> called_;
  };

  EncodedFrameTestObserver post_encode_observer;
  EncodedFrameTestObserver pre_decode_observer;

  test::DirectTransport sender_transport, receiver_transport;

  CreateCalls(Call::Config(&sender_transport),
              Call::Config(&receiver_transport));

  sender_transport.SetReceiver(receiver_call_->Receiver());
  receiver_transport.SetReceiver(sender_call_->Receiver());

  CreateSendConfig(1);
  CreateMatchingReceiveConfigs();
  send_config_.post_encode_callback = &post_encode_observer;
  receive_configs_[0].pre_decode_callback = &pre_decode_observer;

  CreateStreams();
  Start();

  scoped_ptr<test::FrameGenerator> frame_generator(test::FrameGenerator::Create(
      video_streams_[0].width, video_streams_[0].height));
  send_stream_->Input()->SwapFrame(frame_generator->NextFrame());

  EXPECT_EQ(kEventSignaled, post_encode_observer.Wait())
      << "Timed out while waiting for send-side encoded-frame callback.";

  EXPECT_EQ(kEventSignaled, pre_decode_observer.Wait())
      << "Timed out while waiting for pre-decode encoded-frame callback.";

  post_encode_observer.ExpectEqualFrames(pre_decode_observer);

  Stop();

  sender_transport.StopSending();
  receiver_transport.StopSending();

  DestroyStreams();
}

TEST_F(EndToEndTest, ReceiveStreamSendsRemb) {
  class RembObserver : public test::EndToEndTest {
   public:
    RembObserver() : EndToEndTest(kDefaultTimeoutMs) {}

    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());

      bool received_psfb = false;
      bool received_remb = false;
      RTCPUtility::RTCPPacketTypes packet_type = parser.Begin();
      while (packet_type != RTCPUtility::kRtcpNotValidCode) {
        if (packet_type == RTCPUtility::kRtcpPsfbRembCode) {
          const RTCPUtility::RTCPPacket& packet = parser.Packet();
          EXPECT_EQ(packet.PSFBAPP.SenderSSRC, kReceiverLocalSsrc);
          received_psfb = true;
        } else if (packet_type == RTCPUtility::kRtcpPsfbRembItemCode) {
          const RTCPUtility::RTCPPacket& packet = parser.Packet();
          EXPECT_GT(packet.REMBItem.BitRate, 0u);
          EXPECT_EQ(packet.REMBItem.NumberOfSSRCs, 1u);
          EXPECT_EQ(packet.REMBItem.SSRCs[0], kSendSsrcs[0]);
          received_remb = true;
        }
        packet_type = parser.Iterate();
      }
      if (received_psfb && received_remb)
        observation_complete_->Set();
      return SEND_PACKET;
    }
    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait()) << "Timed out while waiting for a "
                                           "receiver RTCP REMB packet to be "
                                           "sent.";
    }
  } test;

  RunBaseTest(&test);
}

void EndToEndTest::TestXrReceiverReferenceTimeReport(bool enable_rrtr) {
  static const int kNumRtcpReportPacketsToObserve = 5;
  class RtcpXrObserver : public test::EndToEndTest {
   public:
    explicit RtcpXrObserver(bool enable_rrtr)
        : EndToEndTest(kDefaultTimeoutMs),
          enable_rrtr_(enable_rrtr),
          sent_rtcp_sr_(0),
          sent_rtcp_rr_(0),
          sent_rtcp_rrtr_(0),
          sent_rtcp_dlrr_(0) {}

   private:
    // Receive stream should send RR packets (and RRTR packets if enabled).
    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());

      RTCPUtility::RTCPPacketTypes packet_type = parser.Begin();
      while (packet_type != RTCPUtility::kRtcpNotValidCode) {
        if (packet_type == RTCPUtility::kRtcpRrCode) {
          ++sent_rtcp_rr_;
        } else if (packet_type ==
                   RTCPUtility::kRtcpXrReceiverReferenceTimeCode) {
          ++sent_rtcp_rrtr_;
        }
        EXPECT_NE(packet_type, RTCPUtility::kRtcpSrCode);
        EXPECT_NE(packet_type, RTCPUtility::kRtcpXrDlrrReportBlockItemCode);
        packet_type = parser.Iterate();
      }
      return SEND_PACKET;
    }
    // Send stream should send SR packets (and DLRR packets if enabled).
    virtual Action OnSendRtcp(const uint8_t* packet, size_t length) {
      RTCPUtility::RTCPParserV2 parser(packet, length, true);
      EXPECT_TRUE(parser.IsValid());

      RTCPUtility::RTCPPacketTypes packet_type = parser.Begin();
      while (packet_type != RTCPUtility::kRtcpNotValidCode) {
        if (packet_type == RTCPUtility::kRtcpSrCode) {
          ++sent_rtcp_sr_;
        } else if (packet_type == RTCPUtility::kRtcpXrDlrrReportBlockItemCode) {
          ++sent_rtcp_dlrr_;
        }
        EXPECT_NE(packet_type, RTCPUtility::kRtcpXrReceiverReferenceTimeCode);
        packet_type = parser.Iterate();
      }
      if (sent_rtcp_sr_ > kNumRtcpReportPacketsToObserve &&
          sent_rtcp_rr_ > kNumRtcpReportPacketsToObserve) {
        if (enable_rrtr_) {
          EXPECT_GT(sent_rtcp_rrtr_, 0);
          EXPECT_GT(sent_rtcp_dlrr_, 0);
        } else {
          EXPECT_EQ(0, sent_rtcp_rrtr_);
          EXPECT_EQ(0, sent_rtcp_dlrr_);
        }
        observation_complete_->Set();
      }
      return SEND_PACKET;
    }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      (*receive_configs)[0].rtp.rtcp_mode = newapi::kRtcpReducedSize;
      (*receive_configs)[0].rtp.rtcp_xr.receiver_reference_time_report =
          enable_rrtr_;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while waiting for RTCP SR/RR packets to be sent.";
    }

    bool enable_rrtr_;
    int sent_rtcp_sr_;
    int sent_rtcp_rr_;
    int sent_rtcp_rrtr_;
    int sent_rtcp_dlrr_;
  } test(enable_rrtr);

  RunBaseTest(&test);
}

void EndToEndTest::TestSendsSetSsrcs(size_t num_ssrcs,
                                     bool send_single_ssrc_first) {
  class SendsSetSsrcs : public test::EndToEndTest {
   public:
    SendsSetSsrcs(const uint32_t* ssrcs,
                  size_t num_ssrcs,
                  bool send_single_ssrc_first)
        : EndToEndTest(kDefaultTimeoutMs),
          num_ssrcs_(num_ssrcs),
          send_single_ssrc_first_(send_single_ssrc_first),
          ssrcs_to_observe_(num_ssrcs),
          expect_single_ssrc_(send_single_ssrc_first) {
      for (size_t i = 0; i < num_ssrcs; ++i)
        valid_ssrcs_[ssrcs[i]] = true;
    }

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      RTPHeader header;
      EXPECT_TRUE(parser_->Parse(packet, length, &header));

      EXPECT_TRUE(valid_ssrcs_[header.ssrc])
          << "Received unknown SSRC: " << header.ssrc;

      if (!valid_ssrcs_[header.ssrc])
        observation_complete_->Set();

      if (!is_observed_[header.ssrc]) {
        is_observed_[header.ssrc] = true;
        --ssrcs_to_observe_;
        if (expect_single_ssrc_) {
          expect_single_ssrc_ = false;
          observation_complete_->Set();
        }
      }

      if (ssrcs_to_observe_ == 0)
        observation_complete_->Set();

      return SEND_PACKET;
    }

    virtual size_t GetNumStreams() const OVERRIDE { return num_ssrcs_; }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      if (num_ssrcs_ > 1) {
        // Set low simulcast bitrates to not have to wait for bandwidth ramp-up.
        for (size_t i = 0; i < video_streams->size(); ++i) {
          (*video_streams)[i].min_bitrate_bps = 10000;
          (*video_streams)[i].target_bitrate_bps = 15000;
          (*video_streams)[i].max_bitrate_bps = 20000;
        }
      }

      all_streams_ = *video_streams;
      if (send_single_ssrc_first_)
        video_streams->resize(1);
    }

    virtual void OnStreamsCreated(
        VideoSendStream* send_stream,
        const std::vector<VideoReceiveStream*>& receive_streams) OVERRIDE {
      send_stream_ = send_stream;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while waiting for "
          << (send_single_ssrc_first_ ? "first SSRC." : "SSRCs.");

      if (send_single_ssrc_first_) {
        // Set full simulcast and continue with the rest of the SSRCs.
        send_stream_->ReconfigureVideoEncoder(all_streams_, NULL);
        EXPECT_EQ(kEventSignaled, Wait())
            << "Timed out while waiting on additional SSRCs.";
      }
    }

   private:
    std::map<uint32_t, bool> valid_ssrcs_;
    std::map<uint32_t, bool> is_observed_;

    const size_t num_ssrcs_;
    const bool send_single_ssrc_first_;

    size_t ssrcs_to_observe_;
    bool expect_single_ssrc_;

    VideoSendStream* send_stream_;
    std::vector<VideoStream> all_streams_;
  } test(kSendSsrcs, num_ssrcs, send_single_ssrc_first);

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, GetStats) {
  class StatsObserver : public test::EndToEndTest, public I420FrameCallback {
   public:
    StatsObserver()
        : EndToEndTest(kLongTimeoutMs),
          receive_stream_(NULL),
          send_stream_(NULL),
          expected_receive_ssrc_(),
          expected_send_ssrcs_(),
          check_stats_event_(EventWrapper::Create()) {}

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      check_stats_event_->Set();
      return SEND_PACKET;
    }

    virtual Action OnSendRtcp(const uint8_t* packet, size_t length) OVERRIDE {
      check_stats_event_->Set();
      return SEND_PACKET;
    }

    virtual Action OnReceiveRtp(const uint8_t* packet, size_t length) OVERRIDE {
      check_stats_event_->Set();
      return SEND_PACKET;
    }

    virtual Action OnReceiveRtcp(const uint8_t* packet,
                                 size_t length) OVERRIDE {
      check_stats_event_->Set();
      return SEND_PACKET;
    }

    virtual void FrameCallback(I420VideoFrame* video_frame) OVERRIDE {
      // Ensure that we have at least 5ms send side delay.
      int64_t render_time = video_frame->render_time_ms();
      if (render_time > 0)
        video_frame->set_render_time_ms(render_time - 5);
    }

    bool CheckReceiveStats() {
      assert(receive_stream_ != NULL);
      VideoReceiveStream::Stats stats = receive_stream_->GetStats();
      EXPECT_EQ(expected_receive_ssrc_, stats.ssrc);

      // Make sure all fields have been populated.

      receive_stats_filled_["IncomingRate"] |=
          stats.network_frame_rate != 0 || stats.bitrate_bps != 0;

      receive_stats_filled_["FrameCallback"] |= stats.decode_frame_rate != 0;

      receive_stats_filled_["FrameRendered"] |= stats.render_frame_rate != 0;

      receive_stats_filled_["StatisticsUpdated"] |=
          stats.rtcp_stats.cumulative_lost != 0 ||
          stats.rtcp_stats.extended_max_sequence_number != 0 ||
          stats.rtcp_stats.fraction_lost != 0 || stats.rtcp_stats.jitter != 0;

      receive_stats_filled_["DataCountersUpdated"] |=
          stats.rtp_stats.bytes != 0 || stats.rtp_stats.fec_packets != 0 ||
          stats.rtp_stats.header_bytes != 0 || stats.rtp_stats.packets != 0 ||
          stats.rtp_stats.padding_bytes != 0 ||
          stats.rtp_stats.retransmitted_packets != 0;

      receive_stats_filled_["CodecStats"] |=
          stats.avg_delay_ms != 0 || stats.discarded_packets != 0 ||
          stats.key_frames != 0 || stats.delta_frames != 0;

      receive_stats_filled_["CName"] |= stats.c_name == expected_cname_;

      return AllStatsFilled(receive_stats_filled_);
    }

    bool CheckSendStats() {
      assert(send_stream_ != NULL);
      VideoSendStream::Stats stats = send_stream_->GetStats();

      send_stats_filled_["NumStreams"] |=
          stats.substreams.size() == expected_send_ssrcs_.size();

      send_stats_filled_["Delay"] |=
          stats.avg_delay_ms != 0 || stats.max_delay_ms != 0;

      receive_stats_filled_["CName"] |= stats.c_name == expected_cname_;

      for (std::map<uint32_t, StreamStats>::const_iterator it =
               stats.substreams.begin();
           it != stats.substreams.end();
           ++it) {
        EXPECT_TRUE(expected_send_ssrcs_.find(it->first) !=
                    expected_send_ssrcs_.end());

        send_stats_filled_[CompoundKey("IncomingRate", it->first)] |=
            stats.input_frame_rate != 0;

        const StreamStats& stream_stats = it->second;

        send_stats_filled_[CompoundKey("StatisticsUpdated", it->first)] |=
            stream_stats.rtcp_stats.cumulative_lost != 0 ||
            stream_stats.rtcp_stats.extended_max_sequence_number != 0 ||
            stream_stats.rtcp_stats.fraction_lost != 0;

        send_stats_filled_[CompoundKey("DataCountersUpdated", it->first)] |=
            stream_stats.rtp_stats.fec_packets != 0 ||
            stream_stats.rtp_stats.padding_bytes != 0 ||
            stream_stats.rtp_stats.retransmitted_packets != 0 ||
            stream_stats.rtp_stats.packets != 0;

        send_stats_filled_[CompoundKey("BitrateStatisticsObserver",
                                       it->first)] |=
            stream_stats.bitrate_bps != 0;

        send_stats_filled_[CompoundKey("FrameCountObserver", it->first)] |=
            stream_stats.delta_frames != 0 || stream_stats.key_frames != 0;

        send_stats_filled_[CompoundKey("OutgoingRate", it->first)] |=
            stats.encode_frame_rate != 0;
      }

      return AllStatsFilled(send_stats_filled_);
    }

    std::string CompoundKey(const char* name, uint32_t ssrc) {
      std::ostringstream oss;
      oss << name << "_" << ssrc;
      return oss.str();
    }

    bool AllStatsFilled(const std::map<std::string, bool>& stats_map) {
      for (std::map<std::string, bool>::const_iterator it = stats_map.begin();
           it != stats_map.end();
           ++it) {
        if (!it->second)
          return false;
      }
      return true;
    }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      send_config->pre_encode_callback = this;  // Used to inject delay.
      send_config->rtp.c_name = "SomeCName";

      expected_receive_ssrc_ = (*receive_configs)[0].rtp.local_ssrc;
      const std::vector<uint32_t>& ssrcs = send_config->rtp.ssrcs;
      for (size_t i = 0; i < ssrcs.size(); ++i)
        expected_send_ssrcs_.insert(ssrcs[i]);

      expected_cname_ = send_config->rtp.c_name;
    }

    virtual void OnStreamsCreated(
        VideoSendStream* send_stream,
        const std::vector<VideoReceiveStream*>& receive_streams) OVERRIDE {
      send_stream_ = send_stream;
      receive_stream_ = receive_streams[0];
    }

    virtual void PerformTest() OVERRIDE {
      Clock* clock = Clock::GetRealTimeClock();
      int64_t now = clock->TimeInMilliseconds();
      int64_t stop_time = now + test::CallTest::kLongTimeoutMs;
      bool receive_ok = false;
      bool send_ok = false;

      while (now < stop_time) {
        if (!receive_ok)
          receive_ok = CheckReceiveStats();
        if (!send_ok)
          send_ok = CheckSendStats();

        if (receive_ok && send_ok)
          return;

        int64_t time_until_timout_ = stop_time - now;
        if (time_until_timout_ > 0)
          check_stats_event_->Wait(time_until_timout_);
        now = clock->TimeInMilliseconds();
      }

      ADD_FAILURE() << "Timed out waiting for filled stats.";
      for (std::map<std::string, bool>::const_iterator it =
               receive_stats_filled_.begin();
           it != receive_stats_filled_.end();
           ++it) {
        if (!it->second) {
          ADD_FAILURE() << "Missing receive stats: " << it->first;
        }
      }

      for (std::map<std::string, bool>::const_iterator it =
               send_stats_filled_.begin();
           it != send_stats_filled_.end();
           ++it) {
        if (!it->second) {
          ADD_FAILURE() << "Missing send stats: " << it->first;
        }
      }
    }

    VideoReceiveStream* receive_stream_;
    std::map<std::string, bool> receive_stats_filled_;

    VideoSendStream* send_stream_;
    std::map<std::string, bool> send_stats_filled_;

    uint32_t expected_receive_ssrc_;
    std::set<uint32_t> expected_send_ssrcs_;
    std::string expected_cname_;

    scoped_ptr<EventWrapper> check_stats_event_;
  } test;

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, ReceiverReferenceTimeReportEnabled) {
  TestXrReceiverReferenceTimeReport(true);
}

TEST_F(EndToEndTest, ReceiverReferenceTimeReportDisabled) {
  TestXrReceiverReferenceTimeReport(false);
}

TEST_F(EndToEndTest, TestReceivedRtpPacketStats) {
  static const size_t kNumRtpPacketsToSend = 5;
  class ReceivedRtpStatsObserver : public test::EndToEndTest {
   public:
    ReceivedRtpStatsObserver()
        : EndToEndTest(kDefaultTimeoutMs),
          receive_stream_(NULL),
          sent_rtp_(0) {}

   private:
    virtual void OnStreamsCreated(
        VideoSendStream* send_stream,
        const std::vector<VideoReceiveStream*>& receive_streams) OVERRIDE {
      receive_stream_ = receive_streams[0];
    }

    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      if (sent_rtp_ >= kNumRtpPacketsToSend) {
        VideoReceiveStream::Stats stats = receive_stream_->GetStats();
        if (kNumRtpPacketsToSend == stats.rtp_stats.packets) {
          observation_complete_->Set();
        }
        return DROP_PACKET;
      }
      ++sent_rtp_;
      return SEND_PACKET;
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while verifying number of received RTP packets.";
    }

    VideoReceiveStream* receive_stream_;
    uint32_t sent_rtp_;
  } test;

  RunBaseTest(&test);
}

TEST_F(EndToEndTest, SendsSetSsrc) { TestSendsSetSsrcs(1, false); }

TEST_F(EndToEndTest, SendsSetSimulcastSsrcs) {
  TestSendsSetSsrcs(kNumSsrcs, false);
}

TEST_F(EndToEndTest, CanSwitchToUseAllSsrcs) {
  TestSendsSetSsrcs(kNumSsrcs, true);
}

TEST_F(EndToEndTest, RedundantPayloadsTransmittedOnAllSsrcs) {
  class ObserveRedundantPayloads: public test::EndToEndTest {
   public:
    ObserveRedundantPayloads()
        : EndToEndTest(kDefaultTimeoutMs), ssrcs_to_observe_(kNumSsrcs) {
          for(size_t i = 0; i < kNumSsrcs; ++i) {
            registered_rtx_ssrc_[kSendRtxSsrcs[i]] = true;
          }
        }

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      RTPHeader header;
      EXPECT_TRUE(parser_->Parse(packet, length, &header));

      if (!registered_rtx_ssrc_[header.ssrc])
        return SEND_PACKET;

      EXPECT_LE(static_cast<size_t>(header.headerLength + header.paddingLength),
                length);
      const bool packet_is_redundant_payload =
          static_cast<size_t>(header.headerLength + header.paddingLength) <
          length;

      if (!packet_is_redundant_payload)
        return SEND_PACKET;

      if (!observed_redundant_retransmission_[header.ssrc]) {
        observed_redundant_retransmission_[header.ssrc] = true;
        if (--ssrcs_to_observe_ == 0)
          observation_complete_->Set();
      }

      return SEND_PACKET;
    }

    virtual size_t GetNumStreams() const OVERRIDE { return kNumSsrcs; }

    virtual void ModifyConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStream::Config>* receive_configs,
        std::vector<VideoStream>* video_streams) OVERRIDE {
      // Set low simulcast bitrates to not have to wait for bandwidth ramp-up.
      for (size_t i = 0; i < video_streams->size(); ++i) {
        (*video_streams)[i].min_bitrate_bps = 10000;
        (*video_streams)[i].target_bitrate_bps = 15000;
        (*video_streams)[i].max_bitrate_bps = 20000;
      }
      // Significantly higher than max bitrates for all video streams -> forcing
      // padding to trigger redundant padding on all RTX SSRCs.
      send_config->rtp.min_transmit_bitrate_bps = 100000;

      send_config->rtp.rtx.payload_type = kSendRtxPayloadType;
      send_config->rtp.rtx.pad_with_redundant_payloads = true;

      for (size_t i = 0; i < kNumSsrcs; ++i)
        send_config->rtp.rtx.ssrcs.push_back(kSendRtxSsrcs[i]);
    }

    virtual void PerformTest() OVERRIDE {
      EXPECT_EQ(kEventSignaled, Wait())
          << "Timed out while waiting for redundant payloads on all SSRCs.";
    }

   private:
    size_t ssrcs_to_observe_;
    std::map<uint32_t, bool> observed_redundant_retransmission_;
    std::map<uint32_t, bool> registered_rtx_ssrc_;
  } test;

  RunBaseTest(&test);
}

void EndToEndTest::TestRtpStatePreservation(bool use_rtx) {
  static const uint32_t kMaxSequenceNumberGap = 100;
  static const uint64_t kMaxTimestampGap = kDefaultTimeoutMs * 90;
  class RtpSequenceObserver : public test::RtpRtcpObserver {
   public:
    RtpSequenceObserver(bool use_rtx)
        : test::RtpRtcpObserver(kDefaultTimeoutMs),
          crit_(CriticalSectionWrapper::CreateCriticalSection()),
          ssrcs_to_observe_(kNumSsrcs) {
      for (size_t i = 0; i < kNumSsrcs; ++i) {
        configured_ssrcs_[kSendSsrcs[i]] = true;
        if (use_rtx)
          configured_ssrcs_[kSendRtxSsrcs[i]] = true;
      }
    }

    void ResetExpectedSsrcs(size_t num_expected_ssrcs) {
      CriticalSectionScoped lock(crit_.get());
      ssrc_observed_.clear();
      ssrcs_to_observe_ = num_expected_ssrcs;
    }

   private:
    virtual Action OnSendRtp(const uint8_t* packet, size_t length) OVERRIDE {
      RTPHeader header;
      EXPECT_TRUE(parser_->Parse(packet, length, &header));
      const uint32_t ssrc = header.ssrc;
      const uint16_t sequence_number = header.sequenceNumber;
      const uint32_t timestamp = header.timestamp;
      const bool only_padding =
          static_cast<size_t>(header.headerLength + header.paddingLength) ==
          length;

      EXPECT_TRUE(configured_ssrcs_[ssrc])
          << "Received SSRC that wasn't configured: " << ssrc;

      std::map<uint32_t, uint16_t>::iterator it =
          last_observed_sequence_number_.find(header.ssrc);
      if (it == last_observed_sequence_number_.end()) {
        last_observed_sequence_number_[ssrc] = sequence_number;
        last_observed_timestamp_[ssrc] = timestamp;
      } else {
        // Verify sequence numbers are reasonably close.
        uint32_t extended_sequence_number = sequence_number;
        // Check for roll-over.
        if (sequence_number < last_observed_sequence_number_[ssrc])
          extended_sequence_number += 0xFFFFu + 1;
        EXPECT_LE(
            extended_sequence_number - last_observed_sequence_number_[ssrc],
            kMaxSequenceNumberGap)
            << "Gap in sequence numbers ("
            << last_observed_sequence_number_[ssrc] << " -> " << sequence_number
            << ") too large for SSRC: " << ssrc << ".";
        last_observed_sequence_number_[ssrc] = sequence_number;

        // TODO(pbos): Remove this check if we ever have monotonically
        // increasing timestamps. Right now padding packets add a delta which
        // can cause reordering between padding packets and regular packets,
        // hence we drop padding-only packets to not flake.
        if (only_padding) {
          // Verify that timestamps are reasonably close.
          uint64_t extended_timestamp = timestamp;
          // Check for roll-over.
          if (timestamp < last_observed_timestamp_[ssrc])
            extended_timestamp += static_cast<uint64_t>(0xFFFFFFFFu) + 1;
          EXPECT_LE(extended_timestamp - last_observed_timestamp_[ssrc],
                    kMaxTimestampGap)
              << "Gap in timestamps (" << last_observed_timestamp_[ssrc]
              << " -> " << timestamp << ") too large for SSRC: " << ssrc << ".";
        }
        last_observed_timestamp_[ssrc] = timestamp;
      }

      CriticalSectionScoped lock(crit_.get());
      // Wait for media packets on all ssrcs.
      if (!ssrc_observed_[ssrc] && !only_padding) {
        ssrc_observed_[ssrc] = true;
        if (--ssrcs_to_observe_ == 0)
          observation_complete_->Set();
      }

      return SEND_PACKET;
    }

    std::map<uint32_t, uint16_t> last_observed_sequence_number_;
    std::map<uint32_t, uint32_t> last_observed_timestamp_;
    std::map<uint32_t, bool> configured_ssrcs_;

    scoped_ptr<CriticalSectionWrapper> crit_;
    size_t ssrcs_to_observe_ GUARDED_BY(crit_);
    std::map<uint32_t, bool> ssrc_observed_ GUARDED_BY(crit_);
  } observer(use_rtx);

  CreateCalls(Call::Config(observer.SendTransport()),
              Call::Config(observer.ReceiveTransport()));
  observer.SetReceivers(sender_call_->Receiver(), NULL);

  CreateSendConfig(kNumSsrcs);

  if (use_rtx) {
    for (size_t i = 0; i < kNumSsrcs; ++i) {
      send_config_.rtp.rtx.ssrcs.push_back(kSendRtxSsrcs[i]);
    }
    send_config_.rtp.rtx.payload_type = kSendRtxPayloadType;
  }

  // Lower bitrates so that all streams send initially.
  for (size_t i = 0; i < video_streams_.size(); ++i) {
    video_streams_[i].min_bitrate_bps = 10000;
    video_streams_[i].target_bitrate_bps = 15000;
    video_streams_[i].max_bitrate_bps = 20000;
  }

  CreateMatchingReceiveConfigs();

  CreateStreams();
  CreateFrameGeneratorCapturer();

  Start();
  EXPECT_EQ(kEventSignaled, observer.Wait())
      << "Timed out waiting for all SSRCs to send packets.";

  // Test stream resetting more than once to make sure that the state doesn't
  // get set once (this could be due to using std::map::insert for instance).
  for (size_t i = 0; i < 3; ++i) {
    frame_generator_capturer_->Stop();
    sender_call_->DestroyVideoSendStream(send_stream_);

    // Re-create VideoSendStream with only one stream.
    std::vector<VideoStream> one_stream = video_streams_;
    one_stream.resize(1);
    send_stream_ =
        sender_call_->CreateVideoSendStream(send_config_, one_stream, NULL);
    send_stream_->Start();
    CreateFrameGeneratorCapturer();
    frame_generator_capturer_->Start();

    observer.ResetExpectedSsrcs(1);
    EXPECT_EQ(kEventSignaled, observer.Wait())
        << "Timed out waiting for single RTP packet.";

    // Reconfigure back to use all streams.
    send_stream_->ReconfigureVideoEncoder(video_streams_, NULL);
    observer.ResetExpectedSsrcs(kNumSsrcs);
    EXPECT_EQ(kEventSignaled, observer.Wait())
        << "Timed out waiting for all SSRCs to send packets.";

    // Reconfigure down to one stream.
    send_stream_->ReconfigureVideoEncoder(one_stream, NULL);
    observer.ResetExpectedSsrcs(1);
    EXPECT_EQ(kEventSignaled, observer.Wait())
        << "Timed out waiting for single RTP packet.";

    // Reconfigure back to use all streams.
    send_stream_->ReconfigureVideoEncoder(video_streams_, NULL);
    observer.ResetExpectedSsrcs(kNumSsrcs);
    EXPECT_EQ(kEventSignaled, observer.Wait())
        << "Timed out waiting for all SSRCs to send packets.";
  }

  observer.StopSending();

  Stop();
  DestroyStreams();
}

TEST_F(EndToEndTest, RestartingSendStreamPreservesRtpState) {
  TestRtpStatePreservation(false);
}

TEST_F(EndToEndTest, RestartingSendStreamPreservesRtpStatesWithRtx) {
  TestRtpStatePreservation(true);
}

}  // namespace webrtc
