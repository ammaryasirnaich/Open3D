// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2021 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// Contains source code from
// https://github.com/mpromonet/webrtc-streamer
//
// This software is in the public domain, furnished "as is", without technical
// support, and with no warranty, express or implied, as to its usefulness for
// any purpose.
// ----------------------------------------------------------------------------
#pragma once

#include <api/video/i420_buffer.h>
#include <modules/video_coding/h264_sprop_parameter_sets.h>
#include <modules/video_coding/include/video_error_codes.h>

#include <cstring>
#include <vector>

namespace open3d {
namespace visualization {
namespace webrtc_server {

class VideoDecoder : public webrtc::DecodedImageCallback {
private:
    class Frame {
    public:
        Frame() : timestamp_ms_(0) {}
        Frame(const rtc::scoped_refptr<webrtc::EncodedImageBuffer>& content,
              uint64_t timestamp_ms,
              webrtc::VideoFrameType frame_type)
            : content_(content),
              timestamp_ms_(timestamp_ms),
              frame_type_(frame_type) {}

        rtc::scoped_refptr<webrtc::EncodedImageBuffer> content_;
        uint64_t timestamp_ms_;
        webrtc::VideoFrameType frame_type_;
    };

public:
    VideoDecoder(rtc::VideoBroadcaster& broadcaster,
                 const std::map<std::string, std::string>& opts,
                 bool wait)
        : broadcaster_(broadcaster),
          stop_(false),
          wait_(wait),
          previmagets_(0),
          prevts_(0) {}

    virtual ~VideoDecoder() {}

    void DecoderThread() {
        while (!stop_) {
            std::unique_lock<std::mutex> mlock(queue_mutex_);
            while (queue_.empty()) {
                queue_cond_.wait(mlock);
            }
            Frame frame = queue_.front();
            queue_.pop();
            mlock.unlock();

            if (frame.content_.get() != nullptr) {
                RTC_LOG(LS_VERBOSE) << "VideoDecoder::DecoderThread size:"
                                    << frame.content_->size()
                                    << " ts:" << frame.timestamp_ms_;
                ssize_t size = frame.content_->size();

                if (size) {
                    webrtc::EncodedImage input_image;
                    input_image.SetEncodedData(frame.content_);
                    input_image._frameType = frame.frame_type_;
                    input_image.SetTimestamp(
                            frame.timestamp_ms_);  // store time in ms that
                                                   // overflow the 32bits
                    int res = decoder_->Decode(input_image, false,
                                               frame.timestamp_ms_);
                    if (res != WEBRTC_VIDEO_CODEC_OK) {
                        RTC_LOG(LS_ERROR)
                                << "VideoDecoder::DecoderThread failure:"
                                << res;
                    }
                }
            }
        }
    }

    void Start() {
        stop_ = false;
        decoder_thread_ = std::thread(&VideoDecoder::DecoderThread, this);
    }

    void Stop() {
        stop_ = true;
        Frame frame;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_.push(frame);
        }
        queue_cond_.notify_all();
        decoder_thread_.join();
    }

    std::vector<std::vector<uint8_t>> getInitFrames(const std::string& codec,
                                                    const char* sdp) {
        std::vector<std::vector<uint8_t>> frames;

        if (codec == "H264") {
            const char* pattern = "sprop-parameter-sets=";
            const char* sprop = strstr(sdp, pattern);
            if (sprop) {
                std::string sdpstr(sprop + strlen(pattern));
                size_t pos = sdpstr.find_first_of(" ;\r\n");
                if (pos != std::string::npos) {
                    sdpstr.erase(pos);
                }
                webrtc::H264SpropParameterSets sprops;
                if (sprops.DecodeSprop(sdpstr)) {
                    std::vector<uint8_t> sps;
                    sps.insert(sps.end(), H26X_marker,
                               H26X_marker + sizeof(H26X_marker));
                    sps.insert(sps.end(), sprops.sps_nalu().begin(),
                               sprops.sps_nalu().end());
                    frames.push_back(sps);

                    std::vector<uint8_t> pps;
                    pps.insert(pps.end(), H26X_marker,
                               H26X_marker + sizeof(H26X_marker));
                    pps.insert(pps.end(), sprops.pps_nalu().begin(),
                               sprops.pps_nalu().end());
                    frames.push_back(pps);
                } else {
                    RTC_LOG(WARNING) << "Cannot decode SPS:" << sprop;
                }
            }
        }

        return frames;
    }

    void createDecoder(const std::string& codec) {
        webrtc::VideoCodec codec_settings;
        if (codec == "H264") {
            decoder_ = factory_.CreateVideoDecoder(
                    webrtc::SdpVideoFormat(cricket::kH264CodecName));
            codec_settings.codecType = webrtc::VideoCodecType::kVideoCodecH264;
        } else if (codec == "VP9") {
            decoder_ = factory_.CreateVideoDecoder(
                    webrtc::SdpVideoFormat(cricket::kVp9CodecName));
            codec_settings.codecType = webrtc::VideoCodecType::kVideoCodecVP9;
        }
        if (decoder_.get() != nullptr) {
            decoder_->InitDecode(&codec_settings, 2);
            decoder_->RegisterDecodeCompleteCallback(this);
        }
    }

    void destroyDecoder() { decoder_.reset(nullptr); }

    bool hasDecoder() { return (decoder_.get() != nullptr); }

    void PostFrame(
            const rtc::scoped_refptr<webrtc::EncodedImageBuffer>& content,
            uint64_t ts,
            webrtc::VideoFrameType frame_type) {
        Frame frame(content, ts, frame_type);
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_.push(frame);
        }
        queue_cond_.notify_all();
    }

    // Overide webrtc::DecodedImageCallback.
    virtual int32_t Decoded(webrtc::VideoFrame& decodedImage) override {
        int64_t ts = std::chrono::high_resolution_clock::now()
                             .time_since_epoch()
                             .count() /
                     1000 / 1000;

        RTC_LOG(LS_VERBOSE)
                << "VideoDecoder::Decoded size:" << decodedImage.size()
                << " decode ts:" << decodedImage.ntp_time_ms()
                << " source ts:" << ts;

        // Waiting.
        if ((wait_) && (prevts_ != 0)) {
            int64_t periodSource = decodedImage.timestamp() - previmagets_;
            int64_t periodDecode = ts - prevts_;

            RTC_LOG(LS_VERBOSE) << "VideoDecoder::Decoded interframe decode:"
                                << periodDecode << " source:" << periodSource;
            int64_t delayms = periodSource - periodDecode;
            if ((delayms > 0) && (delayms < 1000)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayms));
            }
        }

        broadcaster_.OnFrame(decodedImage);

        previmagets_ = decodedImage.timestamp();
        prevts_ = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count() /
                  1000 / 1000;

        return 1;
    }

    rtc::VideoBroadcaster& broadcaster_;
    webrtc::InternalDecoderFactory factory_;
    std::unique_ptr<webrtc::VideoDecoder> decoder_;

    std::queue<Frame> queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cond_;
    std::thread decoder_thread_;
    bool stop_;

    bool wait_;
    int64_t previmagets_;
    int64_t prevts_;
};

}  // namespace webrtc_server
}  // namespace visualization
}  // namespace open3d