// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "video_player.h"

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <shobjidl.h>
#include <windows.h>

#undef GetCurrentTime

using namespace winrt;

VideoPlayer::VideoPlayer(flutter::FlutterView* view, std::wstring uri,
                         flutter::EncodableMap httpHeaders)
    : VideoPlayer(view) {
  // Create a source resolver to create an IMFMediaSource for the content URL.
  // This will create an instance of an inbuilt OS media source for playback.
  winrt::com_ptr<IMFSourceResolver> sourceResolver;
  THROW_IF_FAILED(MFCreateSourceResolver(sourceResolver.put()));
  constexpr uint32_t sourceResolutionFlags =
      MF_RESOLUTION_MEDIASOURCE | MF_RESOLUTION_READ;
  MF_OBJECT_TYPE objectType = {};

  winrt::com_ptr<IMFMediaSource> mediaSource;
  THROW_IF_FAILED(sourceResolver->CreateObjectFromURL(
      uri.c_str(), sourceResolutionFlags, nullptr, &objectType,
      reinterpret_cast<IUnknown**>(mediaSource.put_void())));

  m_mediaEngineWrapper->Initialize(m_adapter, mediaSource.get());
}

VideoPlayer::VideoPlayer(flutter::FlutterView* view)
    : texture(flutter::GpuSurfaceTexture(
          FlutterDesktopGpuSurfaceType::
              kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
          std::bind(&VideoPlayer::ObtainDescriptorCallback, this,
                    std::placeholders::_1, std::placeholders::_2))) {
  m_adapter.attach(view->GetGraphicsAdapter());
  m_window = view->GetNativeWindow();

  m_mfPlatform.Startup();

  // Callbacks invoked by the media engine wrapper
  auto onInitialized = std::bind(&VideoPlayer::OnMediaInitialized, this);
  auto onError = std::bind(&VideoPlayer::OnMediaError, this,
                           std::placeholders::_1, std::placeholders::_2);
  auto onBufferingStateChanged =
      std::bind(&VideoPlayer::OnMediaStateChange, this, std::placeholders::_1);
  auto onPlaybackEndedCB = std::bind(&VideoPlayer::OnPlaybackEnded, this);

  // Create and initialize the MediaEngineWrapper which manages media playback
  m_mediaEngineWrapper = winrt::make_self<media::MediaEngineWrapper>(
      onInitialized, onError, onBufferingStateChanged, onPlaybackEndedCB,
      nullptr);
}

void VideoPlayer::Init(flutter::PluginRegistrarWindows* registrar,
                       int64_t textureId) {
  _textureId = textureId;

  _eventChannel =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(),
          std::string("flutter.io/videoPlayer/videoEvents") +
              std::to_string(textureId),
          &flutter::StandardMethodCodec::GetInstance());

  _eventChannel->SetStreamHandler(
      std::make_unique<
          flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
          [this](const flutter::EncodableValue* arguments,
                 std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&
                     events)
              -> std::unique_ptr<
                  flutter::StreamHandlerError<flutter::EncodableValue>> {
            this->_eventSink = std::move(events);
            return nullptr;
          },
          [this](const flutter::EncodableValue* arguments)
              -> std::unique_ptr<
                  flutter::StreamHandlerError<flutter::EncodableValue>> {
            this->_eventSink = nullptr;
            return nullptr;
          }));

  _textureRegistry = registrar->texture_registrar();
}

VideoPlayer::~VideoPlayer() { m_valid = false; }

bool VideoPlayer::IsValid() { return m_valid; }

FlutterDesktopGpuSurfaceDescriptor* VideoPlayer::ObtainDescriptorCallback(
    size_t width, size_t height) {
  // Lock buffer mutex to protect texture processing
  std::lock_guard<std::mutex> buffer_lock(m_buffer_mutex);

  m_mediaEngineWrapper->UpdateSurfaceDescriptor(
      static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      [&]() { _textureRegistry->MarkTextureFrameAvailable(_textureId); },
      m_descriptor);

  UpdateVideoSize();

  return &m_descriptor;
}

void VideoPlayer::OnMediaInitialized() {
  // Start playback
  m_mediaEngineWrapper->SeekTo(0);
  if (!this->isInitialized) {
    this->isInitialized = true;
    this->SendInitialized();
  }
}

void VideoPlayer::UpdateVideoSize() {
  auto lock = m_compositionLock.lock();

  RECT rect;
  if (GetWindowRect(m_window, &rect)) {
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    m_windowSize = {static_cast<float>(width), static_cast<float>(height)};
  }

  // If the window has not been initialized yet, use a default size of 640x480
  const bool windowInitialized =
      m_windowSize.Width != 0 && m_windowSize.Height != 0;
  float width = windowInitialized ? m_windowSize.Width : 640;
  float height = windowInitialized ? m_windowSize.Height : 480;

  if (m_mediaEngineWrapper) {
    // Call into media engine wrapper on MTA thread to resize the video surface
    media::RunSyncInMTA([&]() {
      m_mediaEngineWrapper->OnWindowUpdate(static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
    });
  }
}

void VideoPlayer::OnMediaError(MF_MEDIA_ENGINE_ERR error, HRESULT hr) {
  LOG_HR_MSG(hr, "MediaEngine error (%d)", error);
}

void VideoPlayer::OnMediaStateChange(
    media::MediaEngineWrapper::BufferingState bufferingState) {
  if (bufferingState ==
      media::MediaEngineWrapper::BufferingState::HAVE_NOTHING) {
    this->SetBuffering(true);
    this->SendBufferingUpdate();
  } else {
    if (!this->isInitialized) {
      this->isInitialized = true;
      this->SendInitialized();
    }
    this->SetBuffering(false);
  }
}

void VideoPlayer::OnPlaybackEnded() {
  if (this->_eventSink) {
    this->_eventSink->Success(
        flutter::EncodableMap({{flutter::EncodableValue("event"),
                                flutter::EncodableValue("completed")}}));
  }
}

void VideoPlayer::SetBuffering(bool buffering) {
  if (_eventSink) {
    _eventSink->Success(flutter::EncodableMap(
        {{flutter::EncodableValue("event"),
          flutter::EncodableValue(buffering ? "bufferingStart"
                                            : "bufferingEnd")}}));
  }
}

void VideoPlayer::SendInitialized() {
  if (isInitialized) {
    auto event = flutter::EncodableMap(
        {{flutter::EncodableValue("event"),
          flutter::EncodableValue("initialized")},
         {flutter::EncodableValue("duration"),
          flutter::EncodableValue(
              (int64_t)m_mediaEngineWrapper->GetDuration())}});

    uint32_t width;
    uint32_t height;
    m_mediaEngineWrapper->GetNativeVideoSize(width, height);
    event.insert({flutter::EncodableValue("width"),
                  flutter::EncodableValue((int32_t)width)});
    event.insert({flutter::EncodableValue("height"),
                  flutter::EncodableValue((int32_t)height)});

    if (this->_eventSink) {
      _eventSink->Success(event);
    }
  }
}

void VideoPlayer::Dispose() {
  if (isInitialized) {
    m_mediaEngineWrapper->Pause();
  }
  _eventChannel = nullptr;
}

void VideoPlayer::SetLooping(bool isLooping) {
  m_mediaEngineWrapper->SetLooping(isLooping);
}

void VideoPlayer::SetVolume(double volume) {
  m_mediaEngineWrapper->SetVolume((float)volume);
}

void VideoPlayer::SetPlaybackSpeed(double playbackSpeed) {
  m_mediaEngineWrapper->SetPlaybackRate(playbackSpeed);
}

void VideoPlayer::Play() {
  m_mediaEngineWrapper->StartPlayingFrom(m_mediaEngineWrapper->GetMediaTime());
}

void VideoPlayer::Pause() { m_mediaEngineWrapper->Pause(); }

int64_t VideoPlayer::GetPosition() {
  return m_mediaEngineWrapper->GetMediaTime();
}

void VideoPlayer::SendBufferingUpdate() {
  auto values = flutter::EncodableList();
  auto ranges = m_mediaEngineWrapper->GetBufferedRanges();
  for (uint32_t i = 0; i < ranges.size(); i++) {
    auto [start, end] = ranges.at(i);
    values.push_back(
        flutter::EncodableList({flutter::EncodableValue((int64_t)(start)),
                                flutter::EncodableValue((int64_t)(end))}));
  }

  if (this->_eventSink) {
    this->_eventSink->Success(
        flutter::EncodableMap({{flutter::EncodableValue("event"),
                                flutter::EncodableValue("bufferingUpdate")},
                               {flutter::EncodableValue("values"), values}}));
  }
}

void VideoPlayer::SeekTo(int64_t seek) { m_mediaEngineWrapper->SeekTo(seek); }

int64_t VideoPlayer::GetTextureId() { return _textureId; }