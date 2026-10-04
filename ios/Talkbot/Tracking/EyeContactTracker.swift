import ARKit
import AVFoundation
import Combine
import ImageIO
import UIKit
import Vision
import TalkbotCore
import simd

/// No camera frame, landmark, face identifier or gaze coordinate leaves this
/// object. ARKit eye direction is an estimate; Vision fallback follows position.
@MainActor
final class EyeContactTracker: NSObject, ObservableObject, ARSessionDelegate {
    @Published private(set) var point: GazePoint = .center
    @Published private(set) var hasFace = false
    @Published private(set) var eyeContact = false
    @Published private(set) var status = "눈맞춤 꺼짐"
    private let arSession = ARSession()
    private var fallback: FacePositionCapture?
    private var filter = GazeFilter()
    private var active = false
    private var generation = 0
    private var lastUpdate: TimeInterval = 0
    private var expiryTimer: Timer?
    private var orientation: UIInterfaceOrientation = .landscapeRight
    private var viewport = CGSize(width: 844, height: 390)

    override init() {
        super.init()
        arSession.delegate = self
        arSession.delegateQueue = .main
    }

    func setViewport(_ size: CGSize, orientation: UIInterfaceOrientation) {
        guard size.width > 0, size.height > 0 else { return }
        self.viewport = size; self.orientation = orientation
        fallback?.setOrientation(orientation)
    }

    func start() async {
        stop()
        generation += 1
        let current = generation
        let granted = await AVCaptureDevice.requestAccess(for: .video)
        guard current == generation else { return }
        guard granted else { status = "카메라 권한 없음 · 음성 대화 가능"; return }
        active = true
        if ARFaceTrackingConfiguration.isSupported {
            let config = ARFaceTrackingConfiguration()
            config.isLightEstimationEnabled = false
            config.maximumNumberOfTrackedFaces = 1
            arSession.run(config, options: [.resetTracking, .removeExistingAnchors])
            status = "시선 추적 중"
        } else { startFallback() }
        expiryTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                self.filter.tick(now: ProcessInfo.processInfo.systemUptime)
                self.publish()
            }
        }
    }

    func stop() {
        generation += 1; active = false
        arSession.pause(); fallback?.stop(); fallback = nil
        expiryTimer?.invalidate(); expiryTimer = nil
        filter = GazeFilter(); publish(); status = "눈맞춤 꺼짐"
    }

    func session(_ session: ARSession, didUpdate frame: ARFrame) {
        let now = ProcessInfo.processInfo.systemUptime
        guard active, now - lastUpdate >= 1.0 / 15,
              let face = frame.anchors.compactMap({ $0 as? ARFaceAnchor }).first,
              face.isTracked else { return }
        lastUpdate = now
        let eyeLocal = (face.leftEyeTransform.columns.3 + face.rightEyeTransform.columns.3) * 0.5
        let eyes = face.transform * SIMD4<Float>(eyeLocal.x, eyeLocal.y, eyeLocal.z, 1)
        let target = face.transform * SIMD4<Float>(face.lookAtPoint.x, face.lookAtPoint.y, face.lookAtPoint.z, 1)
        let camera = frame.camera.transform.columns.3
        let eyeWorld = SIMD3<Float>(eyes.x, eyes.y, eyes.z)
        let gaze = SIMD3<Float>(target.x - eyes.x, target.y - eyes.y, target.z - eyes.z)
        let towardCamera = SIMD3<Float>(camera.x - eyes.x, camera.y - eyes.y, camera.z - eyes.z)
        guard simd_length(gaze) > 0.001, simd_length(towardCamera) > 0.05 else { return }
        let aligned = simd_dot(simd_normalize(gaze), simd_normalize(towardCamera)) > cos(Float(0.26))
        let projected = frame.camera.projectPoint(eyeWorld, orientation: orientation, viewportSize: viewport)
        // Front-facing UI follows the mirrored location of the child's eyes.
        let x = 1 - Double(projected.x / viewport.width) * 2
        let y = Double(projected.y / viewport.height) * 2 - 1
        filter.update(x: x, y: y, eyeContact: aligned, now: now)
        status = "시선 추적 중"
        publish()
    }

    func session(_ session: ARSession, didFailWithError error: Error) {
        guard active else { return }
        arSession.pause()
        startFallback()
    }
    func sessionWasInterrupted(_ session: ARSession) {
        filter = GazeFilter(); publish(); status = "카메라 일시 중단"
    }
    func sessionInterruptionEnded(_ session: ARSession) {
        guard active else { return }
        Task { await start() }
    }

    private func startFallback() {
        guard fallback == nil else { return }
        let capture = FacePositionCapture()
        capture.setOrientation(orientation)
        let current = generation
        capture.onPoint = { [weak self] x, y in
            Task { @MainActor in
                guard let self, self.active, current == self.generation else { return }
                self.filter.update(x: x, y: y, eyeContact: false, now: ProcessInfo.processInfo.systemUptime)
                self.publish()
            }
        }
        capture.onFailure = { [weak self] in
            Task { @MainActor in
                guard let self, current == self.generation else { return }
                self.status = "카메라 사용 불가 · 음성 대화 가능"
            }
        }
        fallback = capture
        capture.start()
        status = "얼굴 위치 추적 중 · 시선 추정 미지원"
    }
    private func publish() {
        point = filter.point; hasFace = filter.hasFace; eyeContact = filter.lookingAtScreen
    }
}

private final class FacePositionCapture: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    var onPoint: ((Double, Double) -> Void)?
    var onFailure: (() -> Void)?
    private let queue = DispatchQueue(label: "talkbot.face-position", qos: .userInitiated)
    private let session = AVCaptureSession()
    private var orientation: CGImagePropertyOrientation = .upMirrored
    private var lastFrame: TimeInterval = 0
    func setOrientation(_ value: UIInterfaceOrientation) {
        queue.async {
            switch value {
            case .portrait: self.orientation = .leftMirrored
            case .portraitUpsideDown: self.orientation = .rightMirrored
            case .landscapeLeft: self.orientation = .downMirrored
            default: self.orientation = .upMirrored
            }
        }
    }
    func start() {
        queue.async {
            do {
                guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) else {
                    self.onFailure?(); return
                }
                let input = try AVCaptureDeviceInput(device: device)
                self.session.beginConfiguration()
                self.session.sessionPreset = .vga640x480
                let output = AVCaptureVideoDataOutput()
                output.alwaysDiscardsLateVideoFrames = true
                output.setSampleBufferDelegate(self, queue: self.queue)
                guard self.session.canAddInput(input), self.session.canAddOutput(output) else {
                    self.session.commitConfiguration(); self.onFailure?(); return
                }
                self.session.addInput(input); self.session.addOutput(output)
                self.session.commitConfiguration(); self.session.startRunning()
            } catch { self.onFailure?() }
        }
    }
    func stop() { queue.async { self.session.stopRunning() } }
    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastFrame > 0.1, let buffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        lastFrame = now
        let request = VNDetectFaceLandmarksRequest()
        do {
            try VNImageRequestHandler(cvPixelBuffer: buffer, orientation: orientation).perform([request])
            guard let face = request.results?.max(by: {
                $0.boundingBox.width * $0.boundingBox.height < $1.boundingBox.width * $1.boundingBox.height
            }) else { return }
            onPoint?(Double(face.boundingBox.midX) * 2 - 1, 1 - Double(face.boundingBox.midY) * 2)
        } catch { /* Transient frame failure: let the gaze expiry recenter. */ }
    }
}
