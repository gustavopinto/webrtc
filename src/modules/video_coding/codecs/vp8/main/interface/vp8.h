/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*
 * vp8.h
 * WEBRTC VP8 wrapper interface
 */


#ifndef WEBRTC_MODULES_VIDEO_CODING_CODECS_VP8_H_
#define WEBRTC_MODULES_VIDEO_CODING_CODECS_VP8_H_

#include "video_codec_interface.h"

// VPX forward declaration
typedef struct vpx_codec_ctx vpx_codec_ctx_t;
typedef struct vpx_codec_ctx vpx_dec_ctx_t;
typedef struct vpx_codec_enc_cfg vpx_codec_enc_cfg_t;
typedef struct vpx_image vpx_image_t;
typedef struct vpx_ref_frame vpx_ref_frame_t;
struct vpx_codec_cx_pkt;

namespace webrtc
{

/******************************/
/* VP8Encoder class           */
/******************************/
class VP8Encoder : public VideoEncoder
{
public:
    VP8Encoder();
    virtual ~VP8Encoder();

// Free encoder memory.
//
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
    virtual WebRtc_Word32 Release();

// Reset encoder state and prepare for a new call.
//
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
//                               <0 - Errors:
//                                 WEBRTC_VIDEO_CODEC_ERR_PARAMETER
//                                 WEBRTC_VIDEO_CODEC_ERROR
    virtual WebRtc_Word32 Reset();

// Initialize the encoder with the information from the codecSettings
//
// Input:
//          - codecSettings     : Codec settings
//          - numberOfCores     : Number of cores available for the encoder
//          - maxPayloadSize    : The maximum size each payload is allowed
//                                to have. Usually MTU - overhead.
//
// Return value                 : Set bit rate if OK
//                                <0 - Errors:
//                                  WEBRTC_VIDEO_CODEC_ERR_PARAMETER
//                                  WEBRTC_VIDEO_CODEC_ERR_SIZE
//                                  WEBRTC_VIDEO_CODEC_LEVEL_EXCEEDED
//                                  WEBRTC_VIDEO_CODEC_MEMORY
//                                  WEBRTC_VIDEO_CODEC_ERROR
    virtual WebRtc_Word32 InitEncode(const VideoCodec* codecSettings,
                                     WebRtc_Word32 numberOfCores,
                                     WebRtc_UWord32 maxPayloadSize);

// Encode an I420 image (as a part of a video stream). The encoded image
// will be returned to the user through the encode complete callback.
//
// Input:
//          - inputImage        : Image to be encoded
//          - frameTypes        : Frame type to be generated by the encoder.
//
// Return value                 : WEBRTC_VIDEO_CODEC_OK if OK
//                                <0 - Errors:
//                                  WEBRTC_VIDEO_CODEC_ERR_PARAMETER
//                                  WEBRTC_VIDEO_CODEC_MEMORY
//                                  WEBRTC_VIDEO_CODEC_ERROR
//                                  WEBRTC_VIDEO_CODEC_TIMEOUT

    virtual WebRtc_Word32 Encode(const RawImage& inputImage,
                                 const CodecSpecificInfo* codecSpecificInfo,
                                 VideoFrameType frameType);

// Register an encode complete callback object.
//
// Input:
//          - callback         : Callback object which handles encoded images.
//
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
    virtual WebRtc_Word32 RegisterEncodeCompleteCallback(EncodedImageCallback*
                                                         callback);

// Inform the encoder of the new packet loss rate in the network
//
//          - packetLoss       : Fraction lost
//                               (loss rate in percent = 100 * packetLoss / 255)
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK
//                               <0 - Errors:
//                                  WEBRTC_VIDEO_CODEC_ERROR
//
    virtual WebRtc_Word32 SetPacketLoss(WebRtc_UWord32 packetLoss);

// Inform the encoder about the new target bit rate.
//
//          - newBitRate       : New target bit rate
//          - frameRate        : The target frame rate
//
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
    virtual WebRtc_Word32 SetRates(WebRtc_UWord32 newBitRateKbit,
                                   WebRtc_UWord32 frameRate);

// Get version number for the codec.
//
// Input:
//      - version       : Pointer to allocated char buffer.
//      - buflen        : Length of provided char buffer.
//
// Output:
//      - version       : Version number string written to char buffer.
//
// Return value         : >0 - Length of written string.
//                        <0 - WEBRTC_VIDEO_CODEC_ERR_SIZE
    virtual WebRtc_Word32 Version(WebRtc_Word8 *version,
                                  WebRtc_Word32 length) const;
    static WebRtc_Word32  VersionStatic(WebRtc_Word8 *version,
                                        WebRtc_Word32 length);

private:
// Call encoder initialize function and set control settings.
    WebRtc_Word32 InitAndSetControlSettings();

    void PopulateCodecSpecific(CodecSpecificInfo* codec_specific,
                               const vpx_codec_cx_pkt& pkt);

    WebRtc_Word32 GetEncodedFrame(const RawImage& input_image);

#if WEBRTC_LIBVPX_VERSION >= 971
    WebRtc_Word32 GetEncodedPartitions(const RawImage& input_image);
#endif

// Determine maximum target for Intra frames
//
// Input:
//    - optimalBuffersize  : Optimal buffer size
// Return Value            : Max target size for Intra frames represented as
//                           percentage of the per frame bandwidth
    WebRtc_UWord32 MaxIntraTarget(WebRtc_UWord32 optimalBuffersize);

    EncodedImage              _encodedImage;
    EncodedImageCallback*     _encodedCompleteCallback;
    WebRtc_Word32             _width;
    WebRtc_Word32             _height;
    WebRtc_Word32             _maxBitRateKbit;
    WebRtc_UWord32            _maxFrameRate;
    bool                      _inited;
    WebRtc_UWord32            _timeStamp;
    WebRtc_UWord16            _pictureID;
    bool                      _pictureLossIndicationOn;
    bool                      _feedbackModeOn;
    bool                      _nextRefIsGolden;
    bool                      _lastAcknowledgedIsGolden;
    bool                      _haveReceivedAcknowledgement;
    WebRtc_UWord16            _pictureIDLastSentRef;
    WebRtc_UWord16            _pictureIDLastAcknowledgedRef;
    int                       _cpuSpeed;
    WebRtc_UWord32            _rcMaxIntraTarget;
    int                       _tokenPartitions;

    vpx_codec_ctx_t*          _encoder;
    vpx_codec_enc_cfg_t*      _cfg;
    vpx_image_t*              _raw;
};// end of VP8Encoder class

/******************************/
/* VP8Decoder class           */
/******************************/
class VP8Decoder : public VideoDecoder
{
public:
    VP8Decoder();
    virtual ~VP8Decoder();

// Initialize the decoder.
//
// Return value         :  WEBRTC_VIDEO_CODEC_OK.
//                        <0 - Errors:
//                                  WEBRTC_VIDEO_CODEC_ERROR
    virtual WebRtc_Word32 InitDecode(const VideoCodec* inst,
                                     WebRtc_Word32 numberOfCores);

// Decode encoded image (as a part of a video stream). The decoded image
// will be returned to the user through the decode complete callback.
//
// Input:
//          - inputImage        : Encoded image to be decoded
//          - missingFrames     : True if one or more frames have been lost
//                                since the previous decode call.
//          - fragmentation     : Specifies the start and length of each VP8
//                                partition.
//          - codecSpecificInfo : pointer to specific codec data
//          - renderTimeMs      : Render time in Ms
//
// Return value                 : WEBRTC_VIDEO_CODEC_OK if OK
//                                <0 - Errors:
//                                      WEBRTC_VIDEO_CODEC_ERROR
//                                      WEBRTC_VIDEO_CODEC_ERR_PARAMETER
    virtual WebRtc_Word32 Decode(const EncodedImage& inputImage,
                                 bool missingFrames,
                                 const RTPFragmentationHeader* fragmentation,
                                 const CodecSpecificInfo* codecSpecificInfo,
                                 WebRtc_Word64 /*renderTimeMs*/);

// Register a decode complete callback object.
//
// Input:
//          - callback         : Callback object which handles decoded images.
//
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK, < 0 otherwise.
    virtual WebRtc_Word32 RegisterDecodeCompleteCallback(DecodedImageCallback*
                                                         callback);

// Free decoder memory.
//
// Return value                : WEBRTC_VIDEO_CODEC_OK if OK
//                               <0 - Errors:
//                                      WEBRTC_VIDEO_CODEC_ERROR
    virtual WebRtc_Word32 Release();

// Reset decoder state and prepare for a new call.
//
// Return value         : WEBRTC_VIDEO_CODEC_OK.
//                        <0 - Errors:
//                                  WEBRTC_VIDEO_CODEC_UNINITIALIZED
//                                  WEBRTC_VIDEO_CODEC_ERROR
    virtual WebRtc_Word32 Reset();

// Create a copy of the codec and its internal state.
//
// Return value                : A copy of the instance if OK, NULL otherwise.
    virtual VideoDecoder* Copy();

private:
// Copy reference image from this _decoder to the _decoder in copyTo. Set which
// frame type to copy in _refFrame->frame_type before the call to this function.
    int CopyReference(VP8Decoder* copyTo);

    WebRtc_Word32 DecodePartitions(const EncodedImage& input_image,
                                   const RTPFragmentationHeader* fragmentation);

    RawImage                   _decodedImage;
    DecodedImageCallback*      _decodeCompleteCallback;
    bool                       _inited;
    bool                       _feedbackModeOn;
    vpx_dec_ctx_t*             _decoder;
    VideoCodec*                _inst;
    WebRtc_Word32              _numCores;
    EncodedImage               _lastKeyFrame;
    int                        _imageFormat;
    vpx_ref_frame_t*           _refFrame;

};// end of VP8Decoder class

} // namespace webrtc

#endif // WEBRTC_MODULES_VIDEO_CODING_CODECS_VP8_H_
