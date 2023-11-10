// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:math';

import 'package:camera_platform_interface/camera_platform_interface.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:stream_transform/stream_transform.dart';

/// An implementation of [CameraPlatform] for Windows.
class CameraWindows extends CameraPlatform {
  /// Registers the Windows implementation of CameraPlatform.
  static void registerWith() {
    CameraPlatform.instance = CameraWindows();
  }

  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final MethodChannel pluginChannel =
      const MethodChannel('plugins.flutter.io/camera_windows');

  /// Camera specific method channels to allow communicating with specific cameras.
  final Map<int, MethodChannel> _cameraChannels = <int, MethodChannel>{};

  /// The controller that broadcasts events coming from handleCameraMethodCall
  ///
  /// It is a `broadcast` because multiple controllers will connect to
  /// different stream views of this Controller.
  /// This is only exposed for test purposes. It shouldn't be used by clients of
  /// the plugin as it may break or change at any time.
  @visibleForTesting
  final StreamController<CameraEvent> cameraEventStreamController =
      StreamController<CameraEvent>.broadcast();

  /// Returns a stream of camera events for the given [cameraId].
  Stream<CameraEvent> _cameraEvents(int cameraId) =>
      cameraEventStreamController.stream
          .where((CameraEvent event) => event.cameraId == cameraId);

  StreamController<CameraImageData>? _frameStreamController;

  StreamSubscription<dynamic>? _platformImageStreamSubscription;

  @override
  Future<List<CameraDescription>> availableCameras() async {
    try {
      final List<Map<dynamic, dynamic>>? cameras = await pluginChannel
          .invokeListMethod<Map<dynamic, dynamic>>('availableCameras');

      if (cameras == null) {
        return <CameraDescription>[];
      }

      return cameras.map((Map<dynamic, dynamic> camera) {
        return CameraDescription(
          name: camera['name'] as String,
          lensDirection:
              parseCameraLensDirection(camera['lensFacing'] as String),
          sensorOrientation: camera['sensorOrientation'] as int,
        );
      }).toList();
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<int> createCamera(
    CameraDescription cameraDescription,
    ResolutionPreset? resolutionPreset, {
    bool enableAudio = false,
  }) async {
    try {
      // If resolutionPreset is not specified, plugin selects the highest resolution possible.
      final Map<String, dynamic>? reply = await pluginChannel
          .invokeMapMethod<String, dynamic>('create', <String, dynamic>{
        'cameraName': cameraDescription.name,
        'resolutionPreset': _serializeResolutionPreset(resolutionPreset),
        'enableAudio': enableAudio,
      });

      if (reply == null) {
        throw CameraException('System', 'Cannot create camera');
      }

      return reply['cameraId']! as int;
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }
  }

  @override
  Future<void> initializeCamera(
    int cameraId, {
    ImageFormatGroup imageFormatGroup = ImageFormatGroup.unknown,
  }) async {
    final int requestedCameraId = cameraId;

    /// Creates channel for camera events.
    _cameraChannels.putIfAbsent(requestedCameraId, () {
      final MethodChannel channel = MethodChannel(
          'plugins.flutter.io/camera_windows/camera$requestedCameraId');
      channel.setMethodCallHandler(
        (MethodCall call) => handleCameraMethodCall(call, requestedCameraId),
      );
      return channel;
    });

    final Map<String, double>? reply;
    try {
      reply = await pluginChannel.invokeMapMethod<String, double>(
        'initialize',
        <String, dynamic>{
          'cameraId': requestedCameraId,
        },
      );
    } on PlatformException catch (e) {
      throw CameraException(e.code, e.message);
    }

    cameraEventStreamController.add(
      CameraInitializedEvent(
        requestedCameraId,
        reply!['previewWidth']!,
        reply['previewHeight']!,
        ExposureMode.auto,
        false,
        FocusMode.auto,
        false,
      ),
    );
  }

  @override
  Future<void> dispose(int cameraId) async {
    await pluginChannel.invokeMethod<void>(
      'dispose',
      <String, dynamic>{'cameraId': cameraId},
    );

    // Destroy method channel after camera is disposed to be able to handle last messages.
    if (_cameraChannels.containsKey(cameraId)) {
      final MethodChannel? cameraChannel = _cameraChannels[cameraId];
      cameraChannel?.setMethodCallHandler(null);
      _cameraChannels.remove(cameraId);
    }
  }

  @override
  Stream<CameraInitializedEvent> onCameraInitialized(int cameraId) {
    return _cameraEvents(cameraId).whereType<CameraInitializedEvent>();
  }

  @override
  Stream<CameraResolutionChangedEvent> onCameraResolutionChanged(int cameraId) {
    /// Windows API does not automatically change the camera's resolution
    /// during capture so these events are never send from the platform.
    /// Support for changing resolution should be implemented, if support for
    /// requesting resolution change is added to camera platform interface.
    return const Stream<CameraResolutionChangedEvent>.empty();
  }

  @override
  Stream<CameraClosingEvent> onCameraClosing(int cameraId) {
    return _cameraEvents(cameraId).whereType<CameraClosingEvent>();
  }

  @override
  Stream<CameraErrorEvent> onCameraError(int cameraId) {
    return _cameraEvents(cameraId).whereType<CameraErrorEvent>();
  }

  @override
  Stream<VideoRecordedEvent> onVideoRecordedEvent(int cameraId) {
    return _cameraEvents(cameraId).whereType<VideoRecordedEvent>();
  }

  @override
  Stream<DeviceOrientationChangedEvent> onDeviceOrientationChanged() {
    // TODO(jokerttu): Implement device orientation detection, https://github.com/flutter/flutter/issues/97540.
    // Force device orientation to landscape as by default camera plugin uses portraitUp orientation.
    return Stream<DeviceOrientationChangedEvent>.value(
      const DeviceOrientationChangedEvent(DeviceOrientation.landscapeRight),
    );
  }

  @override
  Future<void> lockCaptureOrientation(
    int cameraId,
    DeviceOrientation orientation,
  ) async {
    // TODO(jokerttu): Implement lock capture orientation feature, https://github.com/flutter/flutter/issues/97540.
    throw UnimplementedError('lockCaptureOrientation() is not implemented.');
  }

  @override
  Future<void> unlockCaptureOrientation(int cameraId) async {
    // TODO(jokerttu): Implement unlock capture orientation feature, https://github.com/flutter/flutter/issues/97540.
    throw UnimplementedError('unlockCaptureOrientation() is not implemented.');
  }

  @override
  Future<XFile> takePicture(int cameraId) async {
    final String? path;
    path = await pluginChannel.invokeMethod<String>(
      'takePicture',
      <String, dynamic>{'cameraId': cameraId},
    );

    return XFile(path!);
  }

  @override
  Future<void> prepareForVideoRecording() =>
      pluginChannel.invokeMethod<void>('prepareForVideoRecording');

  @override
  Future<void> startVideoRecording(int cameraId,
      {Duration? maxVideoDuration}) async {
    return startVideoCapturing(
        VideoCaptureOptions(cameraId, maxDuration: maxVideoDuration));
  }

  @override
  Future<void> startVideoCapturing(VideoCaptureOptions options) async {
    if (options.streamCallback != null || options.streamOptions != null) {
      throw UnimplementedError(
          'Streaming is not currently supported on Windows');
    }

    await pluginChannel.invokeMethod<void>(
      'startVideoRecording',
      <String, dynamic>{
        'cameraId': options.cameraId,
        'maxVideoDuration': options.maxDuration?.inMilliseconds,
        // 'enableStream': options.streamCallback != null,
      },
    );

    // if (options.streamCallback != null) {
    //   _installStreamController().stream.listen(options.streamCallback);
    //   _startStreamListener();
    // }
  }

  @override
  Future<XFile> stopVideoRecording(int cameraId) async {
    final String? path;

    path = await pluginChannel.invokeMethod<String>(
      'stopVideoRecording',
      <String, dynamic>{'cameraId': cameraId},
    );

    return XFile(path!);
  }

  @override
  Future<void> pauseVideoRecording(int cameraId) async {
    throw UnsupportedError(
        'pauseVideoRecording() is not supported due to Win32 API limitations.');
  }

  @override
  Future<void> resumeVideoRecording(int cameraId) async {
    throw UnsupportedError(
        'resumeVideoRecording() is not supported due to Win32 API limitations.');
  }

  int? camid;
  @override
  Stream<CameraImageData> onStreamedFrameAvailable(int cameraId,
      {CameraImageStreamOptions? options}) {
    camid = cameraId;
    _installStreamController(onListen: _onFrameStreamListen);
    return _frameStreamController!.stream;
  }

  StreamController<CameraImageData> _installStreamController(
      {Function()? onListen}) {
    _frameStreamController = StreamController<CameraImageData>(
      onListen: onListen ?? () {},
      onPause: _onFrameStreamPauseResume,
      onResume: _onFrameStreamPauseResume,
      onCancel: _onFrameStreamCancel,
    );
    return _frameStreamController!;
  }

  void _onFrameStreamListen() {
    _startPlatformStream();
  }

  // StreamController<img.Image>? ImageStream;
  Future<void> _startPlatformStream() async {
    print("here");

    _startStreamListener();
    // await Future.delayed(const Duration(milliseconds: 100));
    try {
      pluginChannel.invokeMethod<void>(
          'startImageStream', <String, dynamic>{'cameraId': camid});
      print("Successfully started!!!!!!!");
    } on PlatformException catch (e) {
      print("Platforme xception noooo: $e");
    }

    // ImageStream = StreamController<img.Image>();
    // return ImageStream!.stream.asBroadcastStream();
  }

  void _startStreamListener() {
    const MethodChannel cameraEventChannel =
        MethodChannel('plugins.flutter.io/camera_windows/imageStream');
    Stopwatch stopwatch = Stopwatch();
    cameraEventChannel.setMethodCallHandler((MethodCall call) {

      List<int> frame = (call.arguments)['data'] as List<int>;
      int width = (call.arguments)['width'] as int;
      int height = (call.arguments)['height'] as int;
      
      print("sanity check");

      // TODO: Need to create a CameraImageData object and drop it into the _frameStreamController.stream

      List<int> rBytes = [];
      List<int> gBytes = [];
      List<int> bBytes = [];
      List<int> aBytes = [];

      for (int i = 0; i + 4 < frame.length; i += 4) {
        if (!(frame[i] == 0 &&
            frame[i + 1] == 0 &&
            frame[i + 2] == 0 &&
            frame[i + 3] != 255)) {
          try {
            rBytes.add(frame[i]);
            gBytes.add(frame[i + 1]);
            bBytes.add(frame[i + 2]);
            aBytes.add(frame[i + 3]);
          } catch (e) {
            print(e.toString());
          }
          // print("setting frame");
        }
      }
      Uint8List rUint8List = Uint8List.fromList(rBytes);
      Uint8List gUint8List = Uint8List.fromList(gBytes);
      Uint8List bUint8List = Uint8List.fromList(bBytes);
      Uint8List aUint8List = Uint8List.fromList(aBytes);

      CameraImagePlane rPlane = CameraImagePlane(
        bytes: rUint8List,
        bytesPerRow: width,
        width: width,
        height: height,
      );
      CameraImagePlane gPlane = CameraImagePlane(
        bytes: gUint8List,
        bytesPerRow: width,
        width: width,
        height: height,
      );
      CameraImagePlane bPlane = CameraImagePlane(
        bytes: bUint8List,
        bytesPerRow: width,
        width: width,
        height: height,
      );
      CameraImagePlane aPlane = CameraImagePlane(
        bytes: aUint8List,
        bytesPerRow: width,
        width: width,
        height: height,
      );

      CameraImageData myImageData = CameraImageData(
          format: CameraImageFormat(
            ImageFormatGroup.bgra8888,
            raw: 42, //This represents RGBA on Android?
          ),
          planes: [rPlane, gPlane, bPlane, aPlane],
          height: height,
          width: width);

      _frameStreamController!.sink.add(myImageData);

      return Future(() => "test");
    });
  }

  FutureOr<void> _onFrameStreamCancel() async {
    await pluginChannel.invokeMethod<void>('stopImageStream');
    await _platformImageStreamSubscription?.cancel();
    _platformImageStreamSubscription = null;
    _frameStreamController = null;
  }

  void _onFrameStreamPauseResume() {
    throw CameraException('InvalidCall',
        'Pause and resume are not supported for onStreamedFrameAvailable');
  }

  @override
  Future<void> setFlashMode(int cameraId, FlashMode mode) async {
    // TODO(jokerttu): Implement flash mode support, https://github.com/flutter/flutter/issues/97537.
    throw UnimplementedError('setFlashMode() is not implemented.');
  }

  @override
  Future<void> setExposureMode(int cameraId, ExposureMode mode) async {
    // TODO(jokerttu): Implement explosure mode support, https://github.com/flutter/flutter/issues/97537.
    throw UnimplementedError('setExposureMode() is not implemented.');
  }

  @override
  Future<void> setExposurePoint(int cameraId, Point<double>? point) async {
    assert(point == null || point.x >= 0 && point.x <= 1);
    assert(point == null || point.y >= 0 && point.y <= 1);

    throw UnsupportedError(
        'setExposurePoint() is not supported due to Win32 API limitations.');
  }

  @override
  Future<double> getMinExposureOffset(int cameraId) async {
    // TODO(jokerttu): Implement exposure control support, https://github.com/flutter/flutter/issues/97537.
    // Value is returned to support existing implementations.
    return 0.0;
  }

  @override
  Future<double> getMaxExposureOffset(int cameraId) async {
    // TODO(jokerttu): Implement exposure control support, https://github.com/flutter/flutter/issues/97537.
    // Value is returned to support existing implementations.
    return 0.0;
  }

  @override
  Future<double> getExposureOffsetStepSize(int cameraId) async {
    // TODO(jokerttu): Implement exposure control support, https://github.com/flutter/flutter/issues/97537.
    // Value is returned to support existing implementations.
    return 1.0;
  }

  @override
  Future<double> setExposureOffset(int cameraId, double offset) async {
    // TODO(jokerttu): Implement exposure control support, https://github.com/flutter/flutter/issues/97537.
    throw UnimplementedError('setExposureOffset() is not implemented.');
  }

  @override
  Future<void> setFocusMode(int cameraId, FocusMode mode) async {
    // TODO(jokerttu): Implement focus mode support, https://github.com/flutter/flutter/issues/97537.
    throw UnimplementedError('setFocusMode() is not implemented.');
  }

  @override
  Future<void> setFocusPoint(int cameraId, Point<double>? point) async {
    assert(point == null || point.x >= 0 && point.x <= 1);
    assert(point == null || point.y >= 0 && point.y <= 1);

    throw UnsupportedError(
        'setFocusPoint() is not supported due to Win32 API limitations.');
  }

  @override
  Future<double> getMinZoomLevel(int cameraId) async {
    // TODO(jokerttu): Implement zoom level support, https://github.com/flutter/flutter/issues/97537.
    // Value is returned to support existing implementations.
    return 1.0;
  }

  @override
  Future<double> getMaxZoomLevel(int cameraId) async {
    // TODO(jokerttu): Implement zoom level support, https://github.com/flutter/flutter/issues/97537.
    // Value is returned to support existing implementations.
    return 1.0;
  }

  @override
  Future<void> setZoomLevel(int cameraId, double zoom) async {
    // TODO(jokerttu): Implement zoom level support, https://github.com/flutter/flutter/issues/97537.
    throw UnimplementedError('setZoomLevel() is not implemented.');
  }

  @override
  Future<void> pausePreview(int cameraId) async {
    await pluginChannel.invokeMethod<double>(
      'pausePreview',
      <String, dynamic>{'cameraId': cameraId},
    );
  }

  @override
  Future<void> resumePreview(int cameraId) async {
    await pluginChannel.invokeMethod<double>(
      'resumePreview',
      <String, dynamic>{'cameraId': cameraId},
    );
  }

  @override
  Widget buildPreview(int cameraId) {
    return Texture(textureId: cameraId);
  }

  /// Returns the resolution preset as a nullable String.
  String? _serializeResolutionPreset(ResolutionPreset? resolutionPreset) {
    switch (resolutionPreset) {
      case null:
        return null;
      case ResolutionPreset.max:
        return 'max';
      case ResolutionPreset.ultraHigh:
        return 'ultraHigh';
      case ResolutionPreset.veryHigh:
        return 'veryHigh';
      case ResolutionPreset.high:
        return 'high';
      case ResolutionPreset.medium:
        return 'medium';
      case ResolutionPreset.low:
        return 'low';
    }
  }

  /// Converts messages received from the native platform into camera events.
  ///
  /// This is only exposed for test purposes. It shouldn't be used by clients
  /// of the plugin as it may break or change at any time.
  @visibleForTesting
  Future<dynamic> handleCameraMethodCall(MethodCall call, int cameraId) async {
    switch (call.method) {
      case 'camera_closing':
        cameraEventStreamController.add(
          CameraClosingEvent(
            cameraId,
          ),
        );
        break;
      case 'video_recorded':
        final Map<String, Object?> arguments =
            (call.arguments as Map<Object?, Object?>).cast<String, Object?>();
        final int? maxDuration = arguments['maxVideoDuration'] as int?;
        // This is called if maxVideoDuration was given on record start.
        cameraEventStreamController.add(
          VideoRecordedEvent(
            cameraId,
            XFile(arguments['path']! as String),
            maxDuration != null ? Duration(milliseconds: maxDuration) : null,
          ),
        );
        break;
      case 'error':
        final Map<String, Object?> arguments =
            (call.arguments as Map<Object?, Object?>).cast<String, Object?>();
        cameraEventStreamController.add(
          CameraErrorEvent(
            cameraId,
            arguments['description']! as String,
          ),
        );
        break;
      default:
        throw UnimplementedError();
    }
  }

  /// Parses string presentation of the camera lens direction and returns enum value.
  @visibleForTesting
  CameraLensDirection parseCameraLensDirection(String string) {
    switch (string) {
      case 'front':
        return CameraLensDirection.front;
      case 'back':
        return CameraLensDirection.back;
      case 'external':
        return CameraLensDirection.external;
    }
    throw ArgumentError('Unknown CameraLensDirection value');
  }
}
