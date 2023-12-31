// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "camera_plugin.h"

#include <flutter/flutter_view.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windows.h>

#include <cassert>
#include <chrono>
#include <memory>

#include "capture_device_info.h"
#include "com_heap_ptr.h"
#include "string_utils.h"

#include <iostream>

namespace camera_windows {
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

namespace {

// Channel events
constexpr char kChannelName[] = "plugins.flutter.io/camera_windows";
constexpr char kImageStreamChannelName[] = "plugins.flutter.io/camera_windows/imageStream";

constexpr char kAvailableCamerasMethod[] = "availableCameras";
constexpr char kCreateMethod[] = "create";
constexpr char kInitializeMethod[] = "initialize";
constexpr char kTakePictureMethod[] = "takePicture";
constexpr char kStartVideoRecordingMethod[] = "startVideoRecording";
constexpr char kStopVideoRecordingMethod[] = "stopVideoRecording";
constexpr char kStartImageStreamMethod[] = "startImageStream";
constexpr char kStopImageStreamMethod[] = "stopImageStream";
constexpr char kPausePreview[] = "pausePreview";
constexpr char kResumePreview[] = "resumePreview";
constexpr char kDisposeMethod[] = "dispose";

constexpr char kCameraNameKey[] = "cameraName";
constexpr char kResolutionPresetKey[] = "resolutionPreset";
constexpr char kEnableAudioKey[] = "enableAudio";

constexpr char kCameraIdKey[] = "cameraId";
constexpr char kMaxVideoDurationKey[] = "maxVideoDuration";

constexpr char kResolutionPresetValueLow[] = "low";
constexpr char kResolutionPresetValueMedium[] = "medium";
constexpr char kResolutionPresetValueHigh[] = "high";
constexpr char kResolutionPresetValueVeryHigh[] = "veryHigh";
constexpr char kResolutionPresetValueUltraHigh[] = "ultraHigh";
constexpr char kResolutionPresetValueMax[] = "max";

const std::string kPictureCaptureExtension = "jpeg";
const std::string kVideoCaptureExtension = "mp4";

// Looks for |key| in |map|, returning the associated value if it is present, or
// a nullptr if not.
const EncodableValue* ValueOrNull(const EncodableMap& map, const char* key) {
  auto it = map.find(EncodableValue(key));
  if (it == map.end()) {
    return nullptr;
  }
  return &(it->second);
}

// Looks for |key| in |map|, returning the associated int64 value if it is
// present, or std::nullopt if not.
std::optional<int64_t> GetInt64ValueOrNull(const EncodableMap& map,
                                           const char* key) {
  auto value = ValueOrNull(map, key);
  if (!value) {
    return std::nullopt;
  }

  if (std::holds_alternative<int32_t>(*value)) {
    return static_cast<int64_t>(std::get<int32_t>(*value));
  }
  auto val64 = std::get_if<int64_t>(value);
  if (!val64) {
    return std::nullopt;
  }
  return *val64;
}

// Parses resolution preset argument to enum value.
ResolutionPreset ParseResolutionPreset(const std::string& resolution_preset) {
  if (resolution_preset.compare(kResolutionPresetValueLow) == 0) {
    return ResolutionPreset::kLow;
  } else if (resolution_preset.compare(kResolutionPresetValueMedium) == 0) {
    return ResolutionPreset::kMedium;
  } else if (resolution_preset.compare(kResolutionPresetValueHigh) == 0) {
    return ResolutionPreset::kHigh;
  } else if (resolution_preset.compare(kResolutionPresetValueVeryHigh) == 0) {
    return ResolutionPreset::kVeryHigh;
  } else if (resolution_preset.compare(kResolutionPresetValueUltraHigh) == 0) {
    return ResolutionPreset::kUltraHigh;
  } else if (resolution_preset.compare(kResolutionPresetValueMax) == 0) {
    return ResolutionPreset::kMax;
  }
  return ResolutionPreset::kAuto;
}

// Builds CaptureDeviceInfo object from given device holding device name and id.
std::unique_ptr<CaptureDeviceInfo> GetDeviceInfo(IMFActivate* device) {
  assert(device);
  auto device_info = std::make_unique<CaptureDeviceInfo>();
  ComHeapPtr<wchar_t> name;
  UINT32 name_size;

  HRESULT hr = device->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                          &name, &name_size);
  if (FAILED(hr)) {
    return device_info;
  }

  ComHeapPtr<wchar_t> id;
  UINT32 id_size;
  hr = device->GetAllocatedString(
      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &id, &id_size);

  if (FAILED(hr)) {
    return device_info;
  }

  device_info->SetDisplayName(Utf8FromUtf16(std::wstring(name, name_size)));
  device_info->SetDeviceID(Utf8FromUtf16(std::wstring(id, id_size)));
  return device_info;
}

// Builds datetime string from current time.
// Used as part of the filenames for captured pictures and videos.
std::string GetCurrentTimeString() {
  std::chrono::system_clock::duration now =
      std::chrono::system_clock::now().time_since_epoch();

  auto s = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 1000;

  struct tm newtime;
  localtime_s(&newtime, &s);

  std::string time_start = "";
  time_start.resize(80);
  size_t len =
      strftime(&time_start[0], time_start.size(), "%Y_%m%d_%H%M%S_", &newtime);
  if (len > 0) {
    time_start.resize(len);
  }

  // Add milliseconds to make sure the filename is unique
  return time_start + std::to_string(ms);
}

// Builds file path for picture capture.
std::optional<std::string> GetFilePathForPicture() {
  ComHeapPtr<wchar_t> known_folder_path;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr,
                                    &known_folder_path);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  std::string path = Utf8FromUtf16(std::wstring(known_folder_path));

  return path + "\\" + "PhotoCapture_" + GetCurrentTimeString() + "." +
         kPictureCaptureExtension;
}

// Builds file path for video capture.
std::optional<std::string> GetFilePathForVideo() {
  ComHeapPtr<wchar_t> known_folder_path;
  HRESULT hr = SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr,
                                    &known_folder_path);
  if (FAILED(hr)) {
    return std::nullopt;
  }

  std::string path = Utf8FromUtf16(std::wstring(known_folder_path));

  return path + "\\" + "VideoCapture_" + GetCurrentTimeString() + "." +
         kVideoCaptureExtension;
}
}  // namespace

std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> streamChanne = nullptr;
// static
void CameraPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto channel = std::make_unique<flutter::MethodChannel<>>(
      registrar->messenger(), kChannelName,
      &flutter::StandardMethodCodec::GetInstance());

  // auto streamChannel = std::make_unique<flutter::MethodChannel<>>(
  //     registrar->messenger(), kImageStreamChannelName,
  //     &flutter::StandardMethodCodec::GetInstance());
streamChanne = std::make_unique<flutter::MethodChannel<>>(registrar->messenger(), kImageStreamChannelName, &flutter::StandardMethodCodec::GetInstance());


// streamChanne->InvokeMethod(kImageStreamChannelName , std::move(std::make_unique<flutter::EncodableValue>("test")));
// auto  streamMethodCall = std::make_unique<flutter::MethodCall<>>("callDartFunction", std::move(std::make_unique<flutter::EncodableValue>("test")));

// auto res = ;//[](const flutter::EncodableValue* result) {}

// auto result_callback = std::make_unique<MyMethodResult>();

// Construct the method call.
      // streamChannel->SetMethodCallHandler(
      // [/*=,frame = flutter_desktop_pixel_buffer_.get()*/](const auto& call, auto result) {
      //   std::cout<<"I'm amazing 1"<<std::endl;
      //   // result->Success(EncodableValue("frame"));
      //   // imgStream = nullptr;
      //   // 
      // });
      // // streamChannel.get()->invokeMethod(kStartImageStreamMethod, EncodableValue("test"), result);
  std::unique_ptr<CameraPlugin> plugin = std::make_unique<CameraPlugin>(
      registrar->texture_registrar(), registrar->messenger());


  channel->SetMethodCallHandler(
      [=,plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });
  streamChanne->SetMethodCallHandler(
      [=,plugin_pointer = plugin.get()](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

CameraPlugin::CameraPlugin(flutter::TextureRegistrar* texture_registrar,
                           flutter::BinaryMessenger* messenger)
    : texture_registrar_(texture_registrar),
      messenger_(messenger),
      camera_factory_(std::make_unique<CameraFactoryImpl>()) {}

CameraPlugin::CameraPlugin(flutter::TextureRegistrar* texture_registrar,
                           flutter::BinaryMessenger* messenger,
                           std::unique_ptr<CameraFactory> camera_factory)
    : texture_registrar_(texture_registrar),
      messenger_(messenger),
      camera_factory_(std::move(camera_factory)) {}

CameraPlugin::~CameraPlugin() {}

void CameraPlugin::HandleMethodCall(
    const flutter::MethodCall<>& method_call,
    std::unique_ptr<flutter::MethodResult<>> result,
    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> streamChannel) {
  const std::string& method_name = method_call.method_name();

  if (method_name.compare(kAvailableCamerasMethod) == 0) {
    return AvailableCamerasMethodHandler(std::move(result));
  } else if (method_name.compare(kCreateMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return CreateMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kInitializeMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return this->InitializeMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kTakePictureMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return TakePictureMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kStartVideoRecordingMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return StartVideoRecordingMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kStopVideoRecordingMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return StopVideoRecordingMethodHandler(*arguments, std::move(result));
  }else if (method_name.compare(kStartImageStreamMethod) == 0) { //start image stream
  std::cout<<"StartImageStream\n";

  streamChannel->InvokeMethod(kImageStreamChannelName , std::move(std::make_unique<flutter::EncodableValue>("test")));
    // const auto* arguments =
    //     std::get_if<flutter::EncodableMap>(method_call.arguments());
    // assert(arguments);

    // return StartImageStreamMethodHandler(*arguments, std::move(result), streamChannel);
  } else if (method_name.compare(kStopImageStreamMethod) == 0) { //stop image stream
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return StopImageStreamMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kPausePreview) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return PausePreviewMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kResumePreview) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return ResumePreviewMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kDisposeMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return DisposeMethodHandler(*arguments, std::move(result));
  } else {
    result->NotImplemented();
  }
}
void CameraPlugin::HandleMethodCall(
    const flutter::MethodCall<>& method_call,
    std::unique_ptr<flutter::MethodResult<>> result) {
  const std::string& method_name = method_call.method_name();
  if(streamChanne!= nullptr){
    std::cout<<"works\n";
    // streamChanne->InvokeMethod(kImageStreamChannelName , std::move(std::make_unique<flutter::EncodableValue>("test")));
  }
  std::cout<<"rip"<<std::endl;
  if (method_name.compare(kAvailableCamerasMethod) == 0) {
    return AvailableCamerasMethodHandler(std::move(result));
  }else if (method_name.compare(kStartImageStreamMethod) == 0 ||method_name.compare("listen") == 0  ) { //start image stream
  std::cout<<"StartImageStream\n";
  std::unique_ptr<EncodableValue> message_data =
        std::make_unique<EncodableValue>(EncodableMap(
            {{EncodableValue("description"), EncodableValue("test")}}));
  // streamChanne->InvokeMethod(kImageStreamChannelName , std::move(std::make_unique<EncodableValue>()));
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    // assert(arguments);

    return StartImageStreamMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kCreateMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return CreateMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kInitializeMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return this->InitializeMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kTakePictureMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return TakePictureMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kStartVideoRecordingMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return StartVideoRecordingMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kStopVideoRecordingMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return StopVideoRecordingMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kPausePreview) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return PausePreviewMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kResumePreview) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return ResumePreviewMethodHandler(*arguments, std::move(result));
  } else if (method_name.compare(kDisposeMethod) == 0) {
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    assert(arguments);

    return DisposeMethodHandler(*arguments, std::move(result));
  } else {
    result->NotImplemented();
  }
}

Camera* CameraPlugin::GetCameraByDeviceId(std::string& device_id) {
  for (auto it = begin(cameras_); it != end(cameras_); ++it) {
    if ((*it)->HasDeviceId(device_id)) {
      return it->get();
    }
  }
  return nullptr;
}

Camera* CameraPlugin::GetCameraByCameraId(int64_t camera_id) {
  for (auto it = begin(cameras_); it != end(cameras_); ++it) {
    if ((*it)->HasCameraId(camera_id)) {
      return it->get();
    }
  }
  return nullptr;
}

void CameraPlugin::DisposeCameraByCameraId(int64_t camera_id) {
  for (auto it = begin(cameras_); it != end(cameras_); ++it) {
    if ((*it)->HasCameraId(camera_id)) {
      cameras_.erase(it);
      return;
    }
  }
}

void CameraPlugin::AvailableCamerasMethodHandler(
    std::unique_ptr<flutter::MethodResult<>> result) {
  // Enumerate devices.
  ComHeapPtr<IMFActivate*> devices;
  UINT32 count = 0;
  if (!this->EnumerateVideoCaptureDeviceSources(&devices, &count)) {
    result->Error("System error", "Failed to get available cameras");
    // No need to free devices here, cos allocation failed.
    return;
  }

  if (count == 0) {
    result->Success(EncodableValue(EncodableList()));
    return;
  }

  // Format found devices to the response.
  EncodableList devices_list;
  for (UINT32 i = 0; i < count; ++i) {
    auto device_info = GetDeviceInfo(devices[i]);
    auto deviceName = device_info->GetUniqueDeviceName();

    devices_list.push_back(EncodableMap({
        {EncodableValue("name"), EncodableValue(deviceName)},
        {EncodableValue("lensFacing"), EncodableValue("front")},
        {EncodableValue("sensorOrientation"), EncodableValue(0)},
    }));
  }

  result->Success(std::move(EncodableValue(devices_list)));
}

bool CameraPlugin::EnumerateVideoCaptureDeviceSources(IMFActivate*** devices,
                                                      UINT32* count) {
  return CaptureControllerImpl::EnumerateVideoCaptureDeviceSources(devices,
                                                                   count);
}

void CameraPlugin::CreateMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  // Parse enableAudio argument.
  const auto* record_audio =
      std::get_if<bool>(ValueOrNull(args, kEnableAudioKey));
  if (!record_audio) {
    return result->Error("argument_error",
                         std::string(kEnableAudioKey) + " argument missing");
  }

  // Parse cameraName argument.
  const auto* camera_name =
      std::get_if<std::string>(ValueOrNull(args, kCameraNameKey));
  if (!camera_name) {
    return result->Error("argument_error",
                         std::string(kCameraNameKey) + " argument missing");
  }

  auto device_info = std::make_unique<CaptureDeviceInfo>();
  if (!device_info->ParseDeviceInfoFromCameraName(*camera_name)) {
    return result->Error(
        "camera_error", "Cannot parse argument " + std::string(kCameraNameKey));
  }

  auto device_id = device_info->GetDeviceId();
  if (GetCameraByDeviceId(device_id)) {
    return result->Error("camera_error",
                         "Camera with given device id already exists. Existing "
                         "camera must be disposed before creating it again.");
  }

  std::unique_ptr<camera_windows::Camera> camera =
      camera_factory_->CreateCamera(device_id);

  if (camera->HasPendingResultByType(PendingResultType::kCreateCamera)) {
    return result->Error("camera_error",
                         "Pending camera creation request exists");
  }

  if (camera->AddPendingResult(PendingResultType::kCreateCamera,
                               std::move(result))) {
    // Parse resolution preset argument.
    const auto* resolution_preset_argument =
        std::get_if<std::string>(ValueOrNull(args, kResolutionPresetKey));
    ResolutionPreset resolution_preset;
    if (resolution_preset_argument) {
      resolution_preset = ParseResolutionPreset(*resolution_preset_argument);
    } else {
      resolution_preset = ResolutionPreset::kAuto;
    }

    bool initialized = camera->InitCamera(texture_registrar_, messenger_,
                                          *record_audio, resolution_preset);
    if (initialized) {
      cameras_.push_back(std::move(camera));
    }
  }
}

void CameraPlugin::InitializeMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }

  if (camera->HasPendingResultByType(PendingResultType::kInitialize)) {
    return result->Error("camera_error",
                         "Pending initialization request exists");
  }

  if (camera->AddPendingResult(PendingResultType::kInitialize,
                               std::move(result))) {
    auto cc = camera->GetCaptureController();
    assert(cc);
    cc->StartPreview();
  }
}

void CameraPlugin::PausePreviewMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }

  if (camera->HasPendingResultByType(PendingResultType::kPausePreview)) {
    return result->Error("camera_error",
                         "Pending pause preview request exists");
  }

  if (camera->AddPendingResult(PendingResultType::kPausePreview,
                               std::move(result))) {
    auto cc = camera->GetCaptureController();
    assert(cc);
    cc->PausePreview();
  }
}

void CameraPlugin::ResumePreviewMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }

  if (camera->HasPendingResultByType(PendingResultType::kResumePreview)) {
    return result->Error("camera_error",
                         "Pending resume preview request exists");
  }

  if (camera->AddPendingResult(PendingResultType::kResumePreview,
                               std::move(result))) {
    auto cc = camera->GetCaptureController();
    assert(cc);
    cc->ResumePreview();
  }
}

void CameraPlugin::StartVideoRecordingMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }

  if (camera->HasPendingResultByType(PendingResultType::kStartRecord)) {
    return result->Error("camera_error",
                         "Pending start recording request exists");
  }

  int64_t max_video_duration_ms = -1;
  auto requested_max_video_duration_ms =
      std::get_if<std::int32_t>(ValueOrNull(args, kMaxVideoDurationKey));

  if (requested_max_video_duration_ms != nullptr) {
    max_video_duration_ms = *requested_max_video_duration_ms;
  }

  std::optional<std::string> path = GetFilePathForVideo();
  if (path) {
    if (camera->AddPendingResult(PendingResultType::kStartRecord,
                                 std::move(result))) {
      auto cc = camera->GetCaptureController();
      assert(cc);
      cc->StartRecord(*path, max_video_duration_ms);
    }
  } else {
    return result->Error("system_error",
                         "Failed to get path for video capture");
  }
}

void CameraPlugin::StopVideoRecordingMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }

  if (camera->HasPendingResultByType(PendingResultType::kStopRecord)) {
    return result->Error("camera_error",
                         "Pending stop recording request exists");
  }

  if (camera->AddPendingResult(PendingResultType::kStopRecord,
                               std::move(result))) {
    auto cc = camera->GetCaptureController();
    assert(cc);
    cc->StopRecord();
  }
}

void CameraPlugin::StartImageStreamMethodHandler(const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result){
  std::cout << "CameraPlugin::StartImageStreamMethodHandler" << std::endl;
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey); //get camera id
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }
  //check if request already exists
  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }
  if (camera->HasPendingResultByType(PendingResultType::kStartStream)) {
    return result->Error("camera_error",
                         "Pending start stream request exists");
  }

  if (!streamChanne) {
    return result->Error("camera_error",
                         "Unable to make method channel from windows");
  }

  if (camera->AddPendingResult(PendingResultType::kStartStream,
                               std::move(result))) {
    auto cc = camera->GetCaptureController();
    assert(cc);
    cc->StartStream(streamChanne.get());
  }
}

void CameraPlugin::StopImageStreamMethodHandler(const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result){
  std::cout<<"StopImageStreamMethodHandler"<<std::endl;
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }
}

void CameraPlugin::TakePictureMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  auto camera = GetCameraByCameraId(*camera_id);
  if (!camera) {
    return result->Error("camera_error", "Camera not created");
  }

  if (camera->HasPendingResultByType(PendingResultType::kTakePicture)) {
    return result->Error("camera_error", "Pending take picture request exists");
  }

  std::optional<std::string> path = GetFilePathForPicture();
  if (path) {
    if (camera->AddPendingResult(PendingResultType::kTakePicture,
                                 std::move(result))) {
      auto cc = camera->GetCaptureController();
      assert(cc);
      cc->TakePicture(*path);
    }
  } else {
    return result->Error("system_error",
                         "Failed to get capture path for picture");
  }
}

void CameraPlugin::DisposeMethodHandler(
    const EncodableMap& args, std::unique_ptr<flutter::MethodResult<>> result) {
  auto camera_id = GetInt64ValueOrNull(args, kCameraIdKey);
  if (!camera_id) {
    return result->Error("argument_error",
                         std::string(kCameraIdKey) + " missing");
  }

  DisposeCameraByCameraId(*camera_id);
  result->Success();
}

}  // namespace camera_windows
