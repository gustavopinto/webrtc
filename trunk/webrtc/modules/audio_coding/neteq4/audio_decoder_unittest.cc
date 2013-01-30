/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_coding/neteq4/audio_decoder_impl.h"

#include <assert.h>
#include <stdlib.h>

#include <string>

#include "gtest/gtest.h"
#include "webrtc/common_audio/resampler/include/resampler.h"
#include "webrtc/modules/audio_coding/codecs/g711/include/g711_interface.h"
#include "webrtc/modules/audio_coding/codecs/g722/include/g722_interface.h"
#include "webrtc/modules/audio_coding/codecs/ilbc/interface/ilbc.h"
#include "webrtc/modules/audio_coding/codecs/isac/fix/interface/isacfix.h"
#include "webrtc/modules/audio_coding/codecs/isac/main/interface/isac.h"
#include "webrtc/modules/audio_coding/codecs/opus/interface/opus_interface.h"
#include "webrtc/modules/audio_coding/codecs/pcm16b/include/pcm16b.h"
#include "webrtc/system_wrappers/interface/data_log.h"
#include "webrtc/test/testsupport/fileutils.h"

namespace webrtc {

class AudioDecoderTest : public ::testing::Test {
 protected:
  AudioDecoderTest()
    : input_fp_(NULL),
      input_(NULL),
      encoded_(NULL),
      decoded_(NULL),
      frame_size_(0),
      data_length_(0),
      encoded_bytes_(0),
      decoder_(NULL) {
    input_file_ = webrtc::test::ProjectRootPath() +
        "resources/audio_coding/testfile32kHz.pcm";
  }

  virtual ~AudioDecoderTest() {}

  virtual void SetUp() {
    // Create arrays.
    ASSERT_GT(data_length_, 0u) << "The test must set data_length_ > 0";
    input_ = new int16_t[data_length_];
    encoded_ = new uint8_t[data_length_ * 2];
    decoded_ = new int16_t[data_length_];
    // Open input file.
    input_fp_ = fopen(input_file_.c_str(), "rb");
    ASSERT_TRUE(input_fp_ != NULL) << "Failed to open file " << input_file_;
    // Read data to |input_|.
    ASSERT_EQ(data_length_,
              fread(input_, sizeof(int16_t), data_length_, input_fp_)) <<
                  "Could not read enough data from file";
    // Logging to view input and output in Matlab.
    // Use 'gyp -Denable_data_logging=1' to enable logging.
    DataLog::CreateLog();
    DataLog::AddTable("CodecTest");
    DataLog::AddColumn("CodecTest", "input", 1);
    DataLog::AddColumn("CodecTest", "output", 1);
  }

  virtual void TearDown() {
    delete decoder_;
    decoder_ = NULL;
    // Close input file.
    fclose(input_fp_);
    // Delete arrays.
    delete [] input_;
    input_ = NULL;
    delete [] encoded_;
    encoded_ = NULL;
    delete [] decoded_;
    decoded_ = NULL;
    // Close log.
    DataLog::ReturnLog();
  }

  virtual void InitEncoder() { }

  // This method must be implemented for all tests derived from this class.
  virtual int EncodeFrame(const int16_t* input, size_t input_len,
                          uint8_t* output) = 0;

  // Encodes and decodes audio. The absolute difference between the input and
  // output is compared vs |tolerance|, and the mean-squared error is compared
  // with |mse|. The encoded stream should contain |expected_bytes|.
  void EncodeDecodeTest(size_t expected_bytes, int tolerance, double mse,
                        int delay = 0) {
    ASSERT_GE(tolerance, 0) << "Test must define a tolerance >= 0";
    size_t processed_samples = 0u;
    encoded_bytes_ = 0u;
    InitEncoder();
    EXPECT_EQ(0, decoder_->Init());
    while (processed_samples + frame_size_ <= data_length_) {
      size_t enc_len = EncodeFrame(&input_[processed_samples], frame_size_,
                                   &encoded_[encoded_bytes_]);
      AudioDecoder::SpeechType speech_type;
      size_t dec_len = decoder_->Decode(&encoded_[encoded_bytes_], enc_len,
                                        &decoded_[processed_samples],
                                        &speech_type);
      EXPECT_EQ(frame_size_, dec_len);
      encoded_bytes_ += enc_len;
      processed_samples += frame_size_;
    }
    EXPECT_EQ(expected_bytes, encoded_bytes_);
    CompareInputOutput(processed_samples, tolerance, delay);
    EXPECT_LE(MseInputOutput(processed_samples, delay), mse);
  }

  // The absolute difference between the input and output is compared vs
  // |tolerance|. The parameter |delay| is used to correct for codec delays.
  void CompareInputOutput(size_t num_samples, int tolerance, int delay) const {
    assert(num_samples <= data_length_);
    for (unsigned int n = 0; n < num_samples - delay; ++n) {
      ASSERT_NEAR(input_[n], decoded_[n + delay], tolerance) <<
          "Exit test on first diff; n = " << n;
      DataLog::InsertCell("CodecTest", "input", input_[n]);
      DataLog::InsertCell("CodecTest", "output", decoded_[n]);
      DataLog::NextRow("CodecTest");
    }
  }

  // Calculates mean-squared error between input and output. The parameter
  // |delay| is used to correct for codec delays.
  double MseInputOutput(size_t num_samples, int delay) const {
    assert(num_samples <= data_length_);
    if (num_samples == 0) return 0.0;

    double squared_sum = 0.0;
    for (unsigned int n = 0; n < num_samples - delay; ++n) {
      squared_sum += (input_[n] - decoded_[n + delay]) *
          (input_[n] - decoded_[n + delay]);
    }
    return squared_sum / (num_samples - delay);
  }

  // Encodes a payload and decodes it twice with decoder re-init before each
  // decode. Verifies that the decoded result is the same.
  void ReInitTest() {
    uint8_t* encoded = encoded_;
    uint8_t* encoded_copy = encoded_ + 2 * frame_size_;
    int16_t* output1 = decoded_;
    int16_t* output2 = decoded_ + frame_size_;
    InitEncoder();
    size_t enc_len = EncodeFrame(input_, frame_size_, encoded);
    // Copy payload since iSAC fix destroys it during decode.
    // Issue: http://code.google.com/p/webrtc/issues/detail?id=845.
    // TODO(hlundin): Remove if the iSAC bug gets fixed.
    memcpy(encoded_copy, encoded, enc_len);
    AudioDecoder::SpeechType speech_type1, speech_type2;
    EXPECT_EQ(0, decoder_->Init());
    size_t dec_len = decoder_->Decode(encoded, enc_len, output1, &speech_type1);
    EXPECT_EQ(frame_size_, dec_len);
    // Re-init decoder and decode again.
    EXPECT_EQ(0, decoder_->Init());
    dec_len = decoder_->Decode(encoded_copy, enc_len, output2, &speech_type2);
    EXPECT_EQ(frame_size_, dec_len);
    for (unsigned int n = 0; n < frame_size_; ++n) {
      ASSERT_EQ(output1[n], output2[n]) << "Exit test on first diff; n = " << n;
    }
    EXPECT_EQ(speech_type1, speech_type2);
  }

  // Call DecodePlc and verify that the correct number of samples is produced.
  void DecodePlcTest() {
    InitEncoder();
    size_t enc_len = EncodeFrame(input_, frame_size_, encoded_);
    AudioDecoder::SpeechType speech_type;
    EXPECT_EQ(0, decoder_->Init());
    size_t dec_len =
        decoder_->Decode(encoded_, enc_len, decoded_, &speech_type);
    EXPECT_EQ(frame_size_, dec_len);
    // Call DecodePlc and verify that we get one frame of data.
    // (Overwrite the output from the above Decode call, but that does not
    // matter.)
    dec_len = decoder_->DecodePlc(1, decoded_);
    EXPECT_EQ(frame_size_, dec_len);
  }

  std::string input_file_;
  FILE* input_fp_;
  int16_t* input_;
  uint8_t* encoded_;
  int16_t* decoded_;
  size_t frame_size_;
  size_t data_length_;
  size_t encoded_bytes_;
  AudioDecoder* decoder_;
};

class AudioDecoderPcmUTest : public AudioDecoderTest {
 protected:
  AudioDecoderPcmUTest() : AudioDecoderTest() {
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderPcmU;
    assert(decoder_);
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    int enc_len_bytes =
        WebRtcG711_EncodeU(NULL, const_cast<int16_t*>(input), input_len_samples,
                           reinterpret_cast<int16_t*>(output));
    EXPECT_EQ(input_len_samples, static_cast<size_t>(enc_len_bytes));
    return enc_len_bytes;
  }
};

class AudioDecoderPcmATest : public AudioDecoderTest {
 protected:
  AudioDecoderPcmATest() : AudioDecoderTest() {
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderPcmA;
    assert(decoder_);
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    int enc_len_bytes =
        WebRtcG711_EncodeA(NULL, const_cast<int16_t*>(input), input_len_samples,
                           reinterpret_cast<int16_t*>(output));
    EXPECT_EQ(input_len_samples, static_cast<size_t>(enc_len_bytes));
    return enc_len_bytes;
  }
};

class AudioDecoderPcm16BTest : public AudioDecoderTest {
 protected:
  AudioDecoderPcm16BTest() : AudioDecoderTest() {
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderPcm16B(kDecoderPCM16B);
    assert(decoder_);
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    int enc_len_bytes = WebRtcPcm16b_EncodeW16(
        const_cast<int16_t*>(input), input_len_samples,
        reinterpret_cast<int16_t*>(output));
    EXPECT_EQ(2 * input_len_samples, static_cast<size_t>(enc_len_bytes));
    return enc_len_bytes;
  }
};

class AudioDecoderIlbcTest : public AudioDecoderTest {
 protected:
  AudioDecoderIlbcTest() : AudioDecoderTest() {
    frame_size_ = 240;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderIlbc;
    assert(decoder_);
    WebRtcIlbcfix_EncoderCreate(&encoder_);
  }

  ~AudioDecoderIlbcTest() {
    WebRtcIlbcfix_EncoderFree(encoder_);
  }

  virtual void InitEncoder() {
    ASSERT_EQ(0, WebRtcIlbcfix_EncoderInit(encoder_, 30));  // 30 ms.
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    int enc_len_bytes =
        WebRtcIlbcfix_Encode(encoder_, input, input_len_samples,
                             reinterpret_cast<int16_t*>(output));
    EXPECT_EQ(50, enc_len_bytes);
    return enc_len_bytes;
  }

  // Overload the default test since iLBC's function WebRtcIlbcfix_NetEqPlc does
  // not return any data. It simply resets a few states and returns 0.
  void DecodePlcTest() {
    InitEncoder();
    size_t enc_len = EncodeFrame(input_, frame_size_, encoded_);
    AudioDecoder::SpeechType speech_type;
    EXPECT_EQ(0, decoder_->Init());
    size_t dec_len =
        decoder_->Decode(encoded_, enc_len, decoded_, &speech_type);
    EXPECT_EQ(frame_size_, dec_len);
    // Simply call DecodePlc and verify that we get 0 as return value.
    EXPECT_EQ(0, decoder_->DecodePlc(1, decoded_));
  }

  iLBC_encinst_t* encoder_;
};

class AudioDecoderIsacFloatTest : public AudioDecoderTest {
 protected:
  AudioDecoderIsacFloatTest() : AudioDecoderTest() {
    input_size_ = 160;
    frame_size_ = 480;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderIsac;
    assert(decoder_);
    WebRtcIsac_Create(&encoder_);
    WebRtcIsac_SetEncSampRate(encoder_, 16000);
  }

  ~AudioDecoderIsacFloatTest() {
    WebRtcIsac_Free(encoder_);
  }

  virtual void InitEncoder() {
    ASSERT_EQ(0, WebRtcIsac_EncoderInit(encoder_, 1));  // Fixed mode.
    ASSERT_EQ(0, WebRtcIsac_Control(encoder_, 32000, 30));  // 32 kbps, 30 ms.
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    // Insert 3 * 10 ms. Expect non-zero output on third call.
    EXPECT_EQ(0, WebRtcIsac_Encode(encoder_, input,
                                   reinterpret_cast<int16_t*>(output)));
    input += input_size_;
    EXPECT_EQ(0, WebRtcIsac_Encode(encoder_, input,
                                   reinterpret_cast<int16_t*>(output)));
    input += input_size_;
    int enc_len_bytes =
        WebRtcIsac_Encode(encoder_, input, reinterpret_cast<int16_t*>(output));
    EXPECT_GT(enc_len_bytes, 0);
    return enc_len_bytes;
  }

  ISACStruct* encoder_;
  int input_size_;
};

class AudioDecoderIsacSwbTest : public AudioDecoderTest {
 protected:
  AudioDecoderIsacSwbTest() : AudioDecoderTest() {
    input_size_ = 320;
    frame_size_ = 960;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderIsacSwb;
    assert(decoder_);
    WebRtcIsac_Create(&encoder_);
    WebRtcIsac_SetEncSampRate(encoder_, 32000);
  }

  ~AudioDecoderIsacSwbTest() {
    WebRtcIsac_Free(encoder_);
  }

  virtual void InitEncoder() {
    ASSERT_EQ(0, WebRtcIsac_EncoderInit(encoder_, 1));  // Fixed mode.
    ASSERT_EQ(0, WebRtcIsac_Control(encoder_, 32000, 30));  // 32 kbps, 30 ms.
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    // Insert 3 * 10 ms. Expect non-zero output on third call.
    EXPECT_EQ(0, WebRtcIsac_Encode(encoder_, input,
                                   reinterpret_cast<int16_t*>(output)));
    input += input_size_;
    EXPECT_EQ(0, WebRtcIsac_Encode(encoder_, input,
                                   reinterpret_cast<int16_t*>(output)));
    input += input_size_;
    int enc_len_bytes =
        WebRtcIsac_Encode(encoder_, input, reinterpret_cast<int16_t*>(output));
    EXPECT_GT(enc_len_bytes, 0);
    return enc_len_bytes;
  }

  ISACStruct* encoder_;
  int input_size_;
};

class AudioDecoderIsacFixTest : public AudioDecoderTest {
 protected:
  AudioDecoderIsacFixTest() : AudioDecoderTest() {
    input_size_ = 160;
    frame_size_ = 480;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderIsacFix;
    assert(decoder_);
    WebRtcIsacfix_Create(&encoder_);
  }

  ~AudioDecoderIsacFixTest() {
    WebRtcIsacfix_Free(encoder_);
  }

  virtual void InitEncoder() {
    ASSERT_EQ(0, WebRtcIsacfix_EncoderInit(encoder_, 1));  // Fixed mode.
    ASSERT_EQ(0,
              WebRtcIsacfix_Control(encoder_, 32000, 30));  // 32 kbps, 30 ms.
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    // Insert 3 * 10 ms. Expect non-zero output on third call.
    EXPECT_EQ(0, WebRtcIsacfix_Encode(encoder_, input,
                                      reinterpret_cast<int16_t*>(output)));
    input += input_size_;
    EXPECT_EQ(0, WebRtcIsacfix_Encode(encoder_, input,
                                      reinterpret_cast<int16_t*>(output)));
    input += input_size_;
    int enc_len_bytes = WebRtcIsacfix_Encode(
        encoder_, input, reinterpret_cast<int16_t*>(output));
    EXPECT_GT(enc_len_bytes, 0);
    return enc_len_bytes;
  }

  ISACFIX_MainStruct* encoder_;
  int input_size_;
};

class AudioDecoderG722Test : public AudioDecoderTest {
 protected:
  AudioDecoderG722Test() : AudioDecoderTest() {
    frame_size_ = 160;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderG722;
    assert(decoder_);
    WebRtcG722_CreateEncoder(&encoder_);
  }

  ~AudioDecoderG722Test() {
    WebRtcG722_FreeEncoder(encoder_);
  }

  virtual void InitEncoder() {
    ASSERT_EQ(0, WebRtcG722_EncoderInit(encoder_));
  }

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    int enc_len_bytes =
        WebRtcG722_Encode(encoder_, const_cast<int16_t*>(input),
                          input_len_samples,
                          reinterpret_cast<int16_t*>(output));
    EXPECT_EQ(80, enc_len_bytes);
    return enc_len_bytes;
  }

  G722EncInst* encoder_;
};

class AudioDecoderOpusTest : public AudioDecoderTest {
 protected:
  AudioDecoderOpusTest() : AudioDecoderTest() {
    frame_size_ = 320;
    data_length_ = 10 * frame_size_;
    decoder_ = new AudioDecoderOpus(kDecoderOpus);
    assert(decoder_);
    WebRtcOpus_EncoderCreate(&encoder_, 1);
  }

  ~AudioDecoderOpusTest() {
    WebRtcOpus_EncoderFree(encoder_);
  }

  virtual void InitEncoder() {}

  virtual int EncodeFrame(const int16_t* input, size_t input_len_samples,
                          uint8_t* output) {
    // Upsample from 32 to 48 kHz.
    Resampler rs;
    rs.Reset(32000, 48000, kResamplerSynchronous);
    const int max_resamp_len_samples = input_len_samples * 3 / 2;
    int16_t* resamp_input = new int16_t[max_resamp_len_samples];
    int resamp_len_samples;
    EXPECT_EQ(0, rs.Push(input, input_len_samples, resamp_input,
                         max_resamp_len_samples, resamp_len_samples));
    EXPECT_EQ(max_resamp_len_samples, resamp_len_samples);
    int enc_len_bytes =
        WebRtcOpus_Encode(encoder_, resamp_input,
                          resamp_len_samples, data_length_, output);
    EXPECT_GT(enc_len_bytes, 0);
    delete [] resamp_input;
    return enc_len_bytes;
  }

  OpusEncInst* encoder_;
};

TEST_F(AudioDecoderPcmUTest, EncodeDecode) {
  int tolerance = 251;
  double mse = 1734.0;
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCMu));
  EncodeDecodeTest(data_length_, tolerance, mse);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderPcmATest, EncodeDecode) {
  int tolerance = 308;
  double mse = 1931.0;
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCMa));
  EncodeDecodeTest(data_length_, tolerance, mse);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderPcm16BTest, EncodeDecode) {
  int tolerance = 0;
  double mse = 0.0;
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16B));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bwb));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bswb32kHz));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bswb48kHz));
  EncodeDecodeTest(2 * data_length_, tolerance, mse);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderIlbcTest, EncodeDecode) {
  int tolerance = 6808;
  double mse = 2.13e6;
  int delay = 80;  // Delay from input to output.
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderILBC));
  EncodeDecodeTest(500, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

TEST_F(AudioDecoderIsacFloatTest, EncodeDecode) {
  int tolerance = 3399;
  double mse = 434951.0;
  int delay = 48;  // Delay from input to output.
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderISAC));
  EncodeDecodeTest(883, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

TEST_F(AudioDecoderIsacSwbTest, EncodeDecode) {
  int tolerance = 19757;
  double mse = 8.18e6;
  int delay = 160;  // Delay from input to output.
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderISACswb));
  EncodeDecodeTest(853, tolerance, mse, delay);
  ReInitTest();
  EXPECT_TRUE(decoder_->HasDecodePlc());
  DecodePlcTest();
}

TEST_F(AudioDecoderIsacFixTest, DISABLED_EncodeDecode) {
  int tolerance = 11034;
  double mse = 3.46e6;
  int delay = 54;  // Delay from input to output.
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderISAC));
  EncodeDecodeTest(735, tolerance, mse, delay);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderG722Test, EncodeDecode) {
  int tolerance = 6176;
  double mse = 238630.0;
  int delay = 22;  // Delay from input to output.
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderG722));
  EncodeDecodeTest(data_length_ / 2, tolerance, mse, delay);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST_F(AudioDecoderOpusTest, EncodeDecode) {
  int tolerance = 6176;
  double mse = 238630.0;
  int delay = 22;  // Delay from input to output.
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderOpus));
  EncodeDecodeTest(731, tolerance, mse, delay);
  ReInitTest();
  EXPECT_FALSE(decoder_->HasDecodePlc());
}

TEST(AudioDecoder, CodecSampleRateHz) {
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCMu));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCMa));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCMu_2ch));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCMa_2ch));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderILBC));
  EXPECT_EQ(16000, AudioDecoder::CodecSampleRateHz(kDecoderISAC));
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderISACswb));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16B));
  EXPECT_EQ(16000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16Bwb));
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16Bswb32kHz));
  EXPECT_EQ(48000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16Bswb48kHz));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16B_2ch));
  EXPECT_EQ(16000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16Bwb_2ch));
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16Bswb32kHz_2ch));
  EXPECT_EQ(48000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16Bswb48kHz_2ch));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderPCM16B_5ch));
  EXPECT_EQ(16000, AudioDecoder::CodecSampleRateHz(kDecoderG722));
  EXPECT_EQ(-1, AudioDecoder::CodecSampleRateHz(kDecoderG722_2ch));
  EXPECT_EQ(-1, AudioDecoder::CodecSampleRateHz(kDecoderRED));
  EXPECT_EQ(-1, AudioDecoder::CodecSampleRateHz(kDecoderAVT));
  EXPECT_EQ(8000, AudioDecoder::CodecSampleRateHz(kDecoderCNGnb));
  EXPECT_EQ(16000, AudioDecoder::CodecSampleRateHz(kDecoderCNGwb));
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderCNGswb32kHz));
  // TODO(tlegrand): Change 32000 to 48000 below once ACM has 48 kHz support.
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderCNGswb48kHz));
  EXPECT_EQ(-1, AudioDecoder::CodecSampleRateHz(kDecoderArbitrary));
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderOpus));
  EXPECT_EQ(32000, AudioDecoder::CodecSampleRateHz(kDecoderOpus_2ch));
  EXPECT_EQ(-1, AudioDecoder::CodecSampleRateHz(kDecoderCELT_32));
  EXPECT_EQ(-1, AudioDecoder::CodecSampleRateHz(kDecoderCELT_32_2ch));
}

TEST(AudioDecoder, CodecSupported) {
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCMu));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCMa));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCMu_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCMa_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderILBC));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderISAC));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderISACswb));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16B));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bwb));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bswb32kHz));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bswb48kHz));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16B_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bwb_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bswb32kHz_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16Bswb48kHz_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderPCM16B_5ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderG722));
  EXPECT_FALSE(AudioDecoder::CodecSupported(kDecoderG722_2ch));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderRED));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderAVT));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderCNGnb));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderCNGwb));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderCNGswb32kHz));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderCNGswb48kHz));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderArbitrary));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderOpus));
  EXPECT_TRUE(AudioDecoder::CodecSupported(kDecoderOpus_2ch));
  EXPECT_FALSE(AudioDecoder::CodecSupported(kDecoderCELT_32));
  EXPECT_FALSE(AudioDecoder::CodecSupported(kDecoderCELT_32_2ch));
}

}  // namespace webrtc
