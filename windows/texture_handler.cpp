// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <flutter/event_channel.h>
#include <flutter/event_sink.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <memory>
#include "texture_handler.h"
#include <iostream>

#include <cassert>

namespace camera_windows {
  using flutter::EncodableValue;
  using flutter::EncodableMap;

TextureHandler::~TextureHandler() {
  // Texture might still be processed while destructor is called.
  // Lock mutex for safe destruction
  const std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (texture_registrar_ && texture_id_ > 0) {
    texture_registrar_->UnregisterTexture(texture_id_);
  }
  texture_id_ = -1;
  texture_ = nullptr;
  texture_registrar_ = nullptr;
}

int64_t TextureHandler::RegisterTexture() {
  if (!texture_registrar_) {
    return -1;
  }

  // Create flutter desktop pixelbuffer texture;
  texture_ =
      std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
          [this](size_t width,
                 size_t height) -> const FlutterDesktopPixelBuffer* {
            return this->ConvertPixelBufferForFlutter(width, height);
          }));

  texture_id_ = texture_registrar_->RegisterTexture(texture_.get());
  return texture_id_;
}

bool TextureHandler::UpdateBuffer(uint8_t* data, uint32_t data_length) {
  // Scoped lock guard.
  {
    const std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!TextureRegistered()) {
      return false;
    }

    if (source_buffer_.size() != data_length) {
      // Update source buffer size.
      source_buffer_.resize(data_length);
    }
    std::copy(data, data + data_length, source_buffer_.data());
  }
  OnBufferUpdated();
  return true;
};
bool TextureHandler::UpdateBuffer(uint8_t* data, uint32_t data_length, flutter::MethodChannel<> *imageStream) {
  // Scoped lock guard.
  {
    // std::cout<<"UpdateBuffer"<<std::endl;
    imgStream = imageStream;
    const std::lock_guard<std::mutex> lock(buffer_mutex_);
    if (!TextureRegistered()) {
      return false;
    }

    if (source_buffer_.size() != data_length) {
      // Update source buffer size.
      source_buffer_.resize(data_length);
    }
    std::copy(data, data + data_length, source_buffer_.data());
  }
  OnBufferUpdated();
  return true;
};


// Marks texture frame available after buffer is updated.
void TextureHandler::OnBufferUpdated() {
  if (TextureRegistered()) {
    texture_registrar_->MarkTextureFrameAvailable(texture_id_);
  }
}

// FlutterDesktopPixel* latest;

// FlutterDesktopPixel* getLatest(){
//   return latest;
// }

const FlutterDesktopPixelBuffer* TextureHandler::ConvertPixelBufferForFlutter(
    size_t target_width, size_t target_height) {
      // std::cout<<target_width+" : "+target_height<<std::endl;
  // TODO: optimize image processing size by adjusting capture size
  // dynamically to match target_width and target_height.
  // If target size changes, create new media type for preview and set new
  // target framesize to MF_MT_FRAME_SIZE attribute.
  // Size should be kept inside requested resolution preset.
  // Update output media type with IMFCaptureSink2::SetOutputMediaType method
  // call and implement IMFCaptureEngineOnSampleCallback2::OnSynchronizedEvent
  // to detect size changes.
  
  // Lock buffer mutex to protect texture processing
  std::unique_lock<std::mutex> buffer_lock(buffer_mutex_);
  if (!TextureRegistered()) {
    return nullptr;
  }

  const uint32_t bytes_per_pixel = 4;
  const uint32_t pixels_total = preview_frame_width_ * preview_frame_height_;
  const uint32_t data_size = pixels_total * bytes_per_pixel;
  if (data_size > 0 && source_buffer_.size() == data_size) {
    if (dest_buffer_.size() != data_size) {
      dest_buffer_.resize(data_size);
    }

    if (r_array.size() != pixels_total) {
      r_array.resize(pixels_total);
      g_array.resize(pixels_total);
      b_array.resize(pixels_total);
      a_array.resize(pixels_total);
    }

    // Map buffers to structs for easier conversion.
    MFVideoFormatRGB32Pixel* src =
        reinterpret_cast<MFVideoFormatRGB32Pixel*>(source_buffer_.data());
    FlutterDesktopPixel* dst =
        reinterpret_cast<FlutterDesktopPixel*>(dest_buffer_.data());

    uint8_t * r_pointer = reinterpret_cast<uint8_t*>(r_array.data());
    uint8_t* g_pointer = reinterpret_cast<uint8_t*>(g_array.data());
    uint8_t* b_pointer = reinterpret_cast<uint8_t*>(b_array.data());
    uint8_t* a_pointer = reinterpret_cast<uint8_t*>(a_array.data());

    for (uint32_t y = 0; y < preview_frame_height_; y++) {
      for (uint32_t x = 0; x < preview_frame_width_; x++) {
        uint32_t sp = (y * preview_frame_width_) + x;
        if (mirror_preview_) {
          // Software mirror mode.
          // IMFCapturePreviewSink also has the SetMirrorState setting,
          // but if enabled, samples will not be processed.

          // Calculates mirrored pixel position.
          uint32_t tp =
              (y * preview_frame_width_) + ((preview_frame_width_ - 1) - x);
          dst[tp].r = src[sp].r;
          dst[tp].g = src[sp].g;
          dst[tp].b = src[sp].b;
          dst[tp].a = 255;

          r_pointer[tp] = src[sp].r;
          g_pointer[tp] = src[sp].g;
          b_pointer[tp] = src[sp].b;
          a_pointer[tp] = 255;
        } else {
          dst[sp].r = src[sp].r;
          dst[sp].g = src[sp].g;
          dst[sp].b = src[sp].b;
          dst[sp].a = 255;

          r_pointer[sp] = src[sp].r;
          g_pointer[sp] = src[sp].g;
          b_pointer[sp] = src[sp].b;
          a_pointer[sp] = 255;
        }
      }
    }

    // latest=dst;
    if (!flutter_desktop_pixel_buffer_) {
      flutter_desktop_pixel_buffer_ =
          std::make_unique<FlutterDesktopPixelBuffer>();

      // Unlocks mutex after texture is processed.
      flutter_desktop_pixel_buffer_->release_callback =
          [](void* release_context) {
            auto mutex = reinterpret_cast<std::mutex*>(release_context);
            mutex->unlock();
          };
    }

    flutter_desktop_pixel_buffer_->buffer = dest_buffer_.data();
    flutter_desktop_pixel_buffer_->width = preview_frame_width_;
    flutter_desktop_pixel_buffer_->height = preview_frame_height_;

    // Releases unique_lock and set mutex pointer for release context.
    flutter_desktop_pixel_buffer_->release_context = buffer_lock.release();
    if(imgStream!=nullptr){
//      std::vector<uint8_t> serializedData(reinterpret_cast<uint8_t*>(dst), reinterpret_cast<uint8_t*>(dst +(preview_frame_height_*preview_frame_width_) ));

      std::vector<uint8_t> serializedDataR(reinterpret_cast<uint8_t*>(r_pointer), reinterpret_cast<uint8_t*>(r_pointer +(preview_frame_height_*preview_frame_width_) ));
      std::vector<uint8_t> serializedDataG(reinterpret_cast<uint8_t*>(g_pointer), reinterpret_cast<uint8_t*>(g_pointer +(preview_frame_height_*preview_frame_width_) ));
      std::vector<uint8_t> serializedDataB(reinterpret_cast<uint8_t*>(b_pointer), reinterpret_cast<uint8_t*>(b_pointer +(preview_frame_height_*preview_frame_width_) ));
      std::vector<uint8_t> serializedDataA(reinterpret_cast<uint8_t*>(a_pointer), reinterpret_cast<uint8_t*>(a_pointer +(preview_frame_height_*preview_frame_width_) ));

      std::unique_ptr<EncodableValue> message_data =
              std::make_unique<EncodableValue>(EncodableMap(
                      {
                              {EncodableValue("height"), EncodableValue(static_cast<int64_t>(preview_frame_height_))},
                              {EncodableValue("width"), EncodableValue(static_cast<int64_t>(preview_frame_width_))},
//                              {EncodableValue("data"), EncodableValue(serializedData)},
                              {EncodableValue("dataR"), EncodableValue(serializedDataR)},
                              {EncodableValue("dataG"), EncodableValue(serializedDataG)},
                              {EncodableValue("dataB"), EncodableValue(serializedDataB)},
                              {EncodableValue("dataA"), EncodableValue(serializedDataA)},
                      }));
      imgStream->InvokeMethod("plugins.flutter.io/camera_windows/imageStream" , std::move(message_data));
//      imgStream->InvokeMethod("plugins.flutter.io/camera_windows/imageStream" , std::move(std::make_unique<EncodableValue>(flutter_desktop_pixel_buffer_.get())));
    }
    return flutter_desktop_pixel_buffer_.get();
  }
  return nullptr;
}

}  // namespace camera_windows
