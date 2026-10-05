import ARKit
import AVFoundation
import Combine
import UIKit
import Vision
import TalkbotCore

/// Front-camera eye landmarks only. No frame, landmark or identifier leaves the device.
@MainActor
final class EyeContactTracker: NSObject, ObservableObject {
    @Published private(set) var point: GazePoint = .center
    @Published private(set) var hasFace = false
    @Published private(set) var eyeContact = false
    @Published private(set) var eyesOpen = true
    @Published private(set) var status = "눈맞춤 꺼짐"

    private var vision: FaceLandmarkCapture?
    private var ar: FaceARCapture?
    private var filter = GazeFilter()
    private var expiryTimer: Timer?
    private var orientation: UIInterfaceOrientation = .portrait
    private var viewport = CGSize(width: 390, height: 844)
    private var generation = 0
    private(set) var running = false

    func setViewport(_ size: CGSize, orientation: UIInterfaceOrientation) {
        guard size.width > 0, size.height > 0 else { return }
        viewport = size
        self.orientation = orientation
        vision?.setOrientation(orientation)
        ar?.setViewport(size, orientation: orientation)
    }

    func start() async {
        if running { return }
        generation += 1
        let current = generation
        let granted = await AVCaptureDevice.requestAccess(for: .video)
        guard current == generation else { return }
        guard granted else {
            status = "카메라 권한 없음"
            return
        }
        running = true
        status = "눈을 찾는 중"
        if ARFaceTrackingConfiguration.isSupported {
            startAR(generation: current)
        } else {
            startVision(generation: current)
        }
        expiryTimer?.invalidate()
        expiryTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.filter.tick(now: ProcessInfo.processInfo.systemUptime)
                self.publish()
            }
        }
    }

    func pauseCapture() {
        vision?.pause()
        ar?.pause()
    }

    func resumeCapture() {
        guard running else { return }
        vision?.resume()
        ar?.resume()
    }

    func stop() {
        generation += 1
        running = false
        vision?.stop()
        vision = nil
        ar?.stop()
        ar = nil
        expiryTimer?.invalidate()
        expiryTimer = nil
        filter = GazeFilter()
        publish()
        status = "눈맞춤 꺼짐"
    }

    private func startAR(generation: Int) {
        let capture = FaceARCapture()
        capture.setViewport(viewport, orientation: orientation)
        capture.onSample = { [weak self] sample in
            Task { @MainActor in
                self?.apply(sample, generation: generation)
            }
        }
        capture.onFailure = { [weak self] in
            Task { @MainActor in
                guard let self, self.running, generation == self.generation else { return }
                self.ar?.stop()
                self.ar = nil
                self.startVision(generation: generation)
            }
        }
        ar = capture
        capture.start()
    }

    private func startVision(generation: Int) {
        let capture = FaceLandmarkCapture()
        capture.setOrientation(orientation)
        capture.onSample = { [weak self] sample in
            Task { @MainActor in
                self?.apply(sample, generation: generation)
            }
        }
        capture.onFailure = { [weak self] in
            Task { @MainActor in
                guard let self, generation == self.generation else { return }
                self.status = "카메라 사용 불가"
            }
        }
        vision = capture
        capture.start()
    }

    private func apply(_ sample: GazeSample, generation: Int) {
        guard running, generation == self.generation else { return }
        filter.update(x: sample.x, y: sample.y, eyeContact: sample.eyeContact,
                      now: ProcessInfo.processInfo.systemUptime, eyesOpen: sample.eyesOpen)
        publish()
        status = sample.eyeContact ? "눈맞춤" : "따라보는 중"
    }

    private func publish() {
        point = filter.point
        hasFace = filter.hasFace
        eyeContact = filter.lookingAtScreen
        eyesOpen = filter.eyesOpen
    }
}

private struct GazeSample {
    var x: Double
    var y: Double
    var eyeContact: Bool
    var eyesOpen: Bool
}

private final class FaceLandmarkCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    var onSample: ((GazeSample) -> Void)?
    var onFailure: (() -> Void)?
    private let queue = DispatchQueue(label: "talkbot.face-landmarks", qos: .userInitiated)
    private let session = AVCaptureSession()
    private var connection: AVCaptureConnection?
    private var lastFrame: TimeInterval = 0
    private var started = false
    private var orientation: UIInterfaceOrientation = .portrait

    func setOrientation(_ value: UIInterfaceOrientation) {
        queue.async {
            self.orientation = value
            self.apply(value)
        }
    }

    func start() {
        queue.async {
            guard !self.started else { return }
            do {
                guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) else {
                    self.onFailure?(); return
                }
                let input = try AVCaptureDeviceInput(device: device)
                let output = AVCaptureVideoDataOutput()
                output.alwaysDiscardsLateVideoFrames = true
                output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
                output.setSampleBufferDelegate(self, queue: self.queue)
                self.session.beginConfiguration()
                self.session.sessionPreset = .vga640x480
                guard self.session.canAddInput(input), self.session.canAddOutput(output) else {
                    self.session.commitConfiguration()
                    self.onFailure?()
                    return
                }
                self.session.addInput(input)
                self.session.addOutput(output)
                self.connection = output.connection(with: .video)
                self.apply(self.orientation)
                self.session.commitConfiguration()
                self.started = true
                self.session.startRunning()
            } catch {
                self.onFailure?()
            }
        }
    }

    func pause() {
        queue.async {
            if self.session.isRunning { self.session.stopRunning() }
        }
    }

    func resume() {
        queue.async {
            guard self.started, !self.session.isRunning else { return }
            self.session.startRunning()
        }
    }

    func stop() {
        queue.async {
            self.session.stopRunning()
            self.started = false
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastFrame > 0.033, let buffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        lastFrame = now
        let request = VNDetectFaceLandmarksRequest()
        do {
            try VNImageRequestHandler(cvPixelBuffer: buffer, orientation: .up).perform([request])
            guard let face = request.results?.max(by: {
                $0.boundingBox.width * $0.boundingBox.height < $1.boundingBox.width * $1.boundingBox.height
            }), let landmarks = face.landmarks else { return }
            let box = face.boundingBox
            let leftEye = landmarks.leftEye
            let rightEye = landmarks.rightEye
            let leftPupil = midpoint(landmarks.leftPupil ?? leftEye, box: box)
            let rightPupil = midpoint(landmarks.rightPupil ?? rightEye, box: box)
            guard let left = leftPupil, let right = rightPupil else { return }
            let head = GazeGeometry.headPoint(midX: Double(box.midX), midY: Double(box.midY))
            let leftLook = GazeGeometry.irisOffset(pupil: left, eye: regionBox(leftEye, box: box))
            let rightLook = GazeGeometry.irisOffset(pupil: right, eye: regionBox(rightEye, box: box))
            let look = GazePoint(x: (leftLook.x + rightLook.x) / 2, y: (leftLook.y + rightLook.y) / 2)
            let gaze = GazeGeometry.combine(head: head, look: look)
            let open = GazeGeometry.eyesOpen(leftSpan: span(landmarks.leftEye), rightSpan: span(landmarks.rightEye))
            let looking = GazeGeometry.lookingAtScreen(point: gaze, eyesOpen: open, faceWidth: Double(box.width))
            onSample?(GazeSample(x: gaze.x, y: gaze.y, eyeContact: looking, eyesOpen: open))
        } catch { }
    }

    private func midpoint(_ region: VNFaceLandmarkRegion2D?, box: CGRect) -> (x: Double, y: Double)? {
        guard let region, region.pointCount > 0 else { return nil }
        var x = 0.0, y = 0.0
        for index in 0..<region.pointCount {
            let point = region.normalizedPoints[index]
            x += Double(box.minX + point.x * box.width)
            y += Double(box.minY + point.y * box.height)
        }
        return (x / Double(region.pointCount), y / Double(region.pointCount))
    }

    private func regionBox(_ region: VNFaceLandmarkRegion2D?, box: CGRect) -> (midX: Double, midY: Double, width: Double, height: Double) {
        guard let region, region.pointCount > 1 else {
            return (midX: Double(box.midX), midY: Double(box.midY), width: Double(box.width) * 0.18, height: Double(box.height) * 0.08)
        }
        var minX = 1.0, maxX = 0.0, minY = 1.0, maxY = 0.0
        for index in 0..<region.pointCount {
            let point = region.normalizedPoints[index]
            let x = Double(box.minX + point.x * box.width)
            let y = Double(box.minY + point.y * box.height)
            minX = min(minX, x); maxX = max(maxX, x)
            minY = min(minY, y); maxY = max(maxY, y)
        }
        return (midX: (minX + maxX) / 2, midY: (minY + maxY) / 2, width: max(maxX - minX, 0.01), height: max(maxY - minY, 0.01))
    }

    private func span(_ region: VNFaceLandmarkRegion2D?) -> Double {
        guard let region, region.pointCount > 1 else { return 0.08 }
        let values = region.normalizedPoints.map { Double($0.y) }
        return (values.max() ?? 0) - (values.min() ?? 0)
    }

    private func apply(_ orientation: UIInterfaceOrientation) {
        guard let connection, connection.isVideoOrientationSupported else { return }
        switch orientation {
        case .landscapeLeft: connection.videoOrientation = .landscapeLeft
        case .portrait: connection.videoOrientation = .portrait
        case .portraitUpsideDown: connection.videoOrientation = .portraitUpsideDown
        default: connection.videoOrientation = .landscapeRight
        }
        if connection.isVideoMirroringSupported {
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = true
        }
    }
}

private final class FaceARCapture: NSObject, ARSessionDelegate {
    var onSample: ((GazeSample) -> Void)?
    var onFailure: (() -> Void)?
    private let session = ARSession()
    private let queue = DispatchQueue(label: "talkbot.face-ar")
    private var lastFrame: TimeInterval = 0
    private var started = false

    func setViewport(_ size: CGSize, orientation: UIInterfaceOrientation) {}

    func start() {
        queue.async {
            guard !self.started, ARFaceTrackingConfiguration.isSupported else {
                self.onFailure?()
                return
            }
            let config = ARFaceTrackingConfiguration()
            config.isLightEstimationEnabled = false
            self.session.delegate = self
            self.session.delegateQueue = self.queue
            self.session.run(config, options: [.resetTracking, .removeExistingAnchors])
            self.started = true
        }
    }

    func pause() {
        queue.async { self.session.pause() }
    }

    func resume() {
        queue.async {
            guard self.started, ARFaceTrackingConfiguration.isSupported else { return }
            self.session.run(ARFaceTrackingConfiguration(), options: [])
        }
    }

    func stop() {
        queue.async {
            self.session.pause()
            self.started = false
        }
    }

    func session(_ session: ARSession, didFailWithError error: Error) {
        onFailure?()
    }

    func session(_ session: ARSession, didUpdate anchors: [ARAnchor]) {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastFrame > 0.033, let face = anchors.compactMap({ $0 as? ARFaceAnchor }).first, face.isTracked else { return }
        lastFrame = now
        let shape = face.blendShapes
        let gaze = GazeGeometry.fromARFace(
            faceX: Double(face.transform.columns.3.x),
            faceY: Double(face.transform.columns.3.y),
            lookOutLeft: shape[.eyeLookOutLeft]?.doubleValue ?? 0,
            lookInLeft: shape[.eyeLookInLeft]?.doubleValue ?? 0,
            lookOutRight: shape[.eyeLookOutRight]?.doubleValue ?? 0,
            lookInRight: shape[.eyeLookInRight]?.doubleValue ?? 0,
            lookUpLeft: shape[.eyeLookUpLeft]?.doubleValue ?? 0,
            lookDownLeft: shape[.eyeLookDownLeft]?.doubleValue ?? 0,
            lookUpRight: shape[.eyeLookUpRight]?.doubleValue ?? 0,
            lookDownRight: shape[.eyeLookDownRight]?.doubleValue ?? 0)
        let blinkL = shape[.eyeBlinkLeft]?.doubleValue ?? 0
        let blinkR = shape[.eyeBlinkRight]?.doubleValue ?? 0
        let open = blinkL < 0.55 && blinkR < 0.55
        let looking = open && hypot(gaze.x, gaze.y) < 0.34
        onSample?(GazeSample(x: gaze.x, y: gaze.y, eyeContact: looking, eyesOpen: open))
    }
}
