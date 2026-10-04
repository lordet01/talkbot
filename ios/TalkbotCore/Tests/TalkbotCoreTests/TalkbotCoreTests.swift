import XCTest
@testable import TalkbotCore

final class TalkbotCoreTests: XCTestCase {
    func testEnglishEllipsisPreservesActivity() {
        var state = ConversationState()
        state.hear("영어놀이 하자")
        state.hear("강아지는?")
        XCTAssertEqual(state.activity, .english)
        XCTAssertEqual(state.topic, "강아지는?")
    }
    func testHintProgressionDoesNotChangeActivity() {
        var state = ConversationState()
        state.hear("영어 공부하자")
        for _ in 0..<6 { state.hear("몰라") }
        XCTAssertEqual(state.activity, .english)
        XCTAssertEqual(state.hintLevel, 3)
    }
    func testRejectEndsGameWithoutEmotionMode() {
        var state = ConversationState()
        state.hear("영어놀이 하자"); state.hear("몰라"); state.hear("싫어")
        XCTAssertEqual(state.activity, .free)
        XCTAssertEqual(state.hintLevel, 0)
        XCTAssertEqual(state.expression, .warm)
    }
    func testStoryContinuesWithoutTopicReset() {
        var state = ConversationState()
        state.hear("토끼 동화 들려줘")
        let topic = state.topic
        state.hear("그다음")
        XCTAssertEqual(state.activity, .story)
        XCTAssertEqual(state.topic, topic)
    }
    func testEmotionAcknowledgedWithoutStealingActivity() {
        var state = ConversationState()
        state.hear("동화 들려줘"); state.hear("무서워")
        XCTAssertEqual(state.activity, .story)
        XCTAssertEqual(state.expression, .comforting)
    }
    func testResponseGateRejectsStaleAudioAfterInterruption() {
        var gate = ResponseGate()
        XCTAssertTrue(gate.begin("old"))
        XCTAssertTrue(gate.accepts("old"))
        gate.interrupt()
        XCTAssertFalse(gate.accepts("old"))
        XCTAssertTrue(gate.begin("new"))
        XCTAssertFalse(gate.accepts("old"))
        XCTAssertFalse(gate.begin("old"))
        XCTAssertTrue(gate.accepts("new"))
        XCTAssertFalse(gate.accepts(nil))
    }
    func testLatencyIncludesEndpointSilenceAndRenderDelay() {
        var latency = TurnLatency()
        latency.start(lastVoice: 10, serverStop: 10.45)
        let sample = latency.rendered(at: 10.95)
        XCTAssertEqual(sample?.milliseconds ?? 0, 950, accuracy: 0.001)
        XCTAssertTrue(sample?.metTarget ?? false)
        XCTAssertNil(latency.rendered(at: 11.2))
    }
    func testLatencyDoesNotHideSlowTurns() {
        var latency = TurnLatency()
        latency.start(lastVoice: 10, serverStop: 10.5)
        latency.tick(now: 11.01)
        XCTAssertTrue(latency.deadlineMissed)
        XCTAssertFalse(latency.rendered(at: 11.2)?.metTarget ?? true)
        XCTAssertEqual(latency.p95 ?? 0, 1200, accuracy: 0.001)
    }
    func testMissingOrStaleLocalEndpointIsNotFakeEndToEndSample() {
        var latency = TurnLatency()
        latency.start(lastVoice: nil, serverStop: 10)
        XCTAssertNil(latency.rendered(at: 10.1))
        latency.start(lastVoice: 2, serverStop: 10)
        XCTAssertNil(latency.rendered(at: 10.1))
        XCTAssertTrue(latency.samples.isEmpty)
    }
    func testInterruptionDoesNotCountAsFailure() {
        var latency = TurnLatency()
        latency.start(lastVoice: 10, serverStop: 10.4)
        latency.abandon()
        XCTAssertNil(latency.rendered(at: 12))
        XCTAssertEqual(latency.timedOutTurns, 0)
        latency.start(lastVoice: 20, serverStop: 20.4)
        latency.abandon(countTimeout: true)
        XCTAssertEqual(latency.timedOutTurns, 1)
    }
    func testLatencyWindowIsBoundedAndP95UsesNearestRank() {
        var latency = TurnLatency()
        for index in 1...105 {
            latency.start(lastVoice: 10, serverStop: 10.01)
            latency.rendered(at: 10 + Double(index) / 1000)
        }
        XCTAssertEqual(latency.samples.count, 100)
        XCTAssertEqual(latency.p95 ?? 0, 100, accuracy: 0.001)
    }
    func testGazeRejectsInvalidCoordinatesAndClamps() {
        var gaze = GazeFilter()
        gaze.update(x: .nan, y: 0, eyeContact: true, now: 10)
        XCTAssertFalse(gaze.hasFace)
        gaze.update(x: 4, y: -4, eyeContact: true, now: 10)
        XCTAssertTrue(gaze.hasFace)
        XCTAssertTrue(gaze.lookingAtScreen)
        XCTAssertLessThanOrEqual(gaze.point.x, 1)
        XCTAssertGreaterThanOrEqual(gaze.point.y, -1)
    }
    func testLostFaceRecentersRatherThanFollowingStalePosition() {
        var gaze = GazeFilter()
        gaze.update(x: 0.7, y: 0.2, eyeContact: true, now: 10)
        gaze.tick(now: 10.5)
        XCTAssertTrue(gaze.hasFace)
        gaze.tick(now: 10.8)
        XCTAssertFalse(gaze.hasFace)
        XCTAssertFalse(gaze.lookingAtScreen)
        XCTAssertEqual(gaze.point, .center)
    }
    func testGAEventDecodingAudioAndDone() throws {
        let audio = try JSONDecoder().decode(ServerEvent.self, from: Data("""
        {"type":"response.output_audio.delta","response_id":"r1","item_id":"i1","content_index":0,"delta":"AAA="}
        """.utf8))
        XCTAssertEqual(audio.responseID, "r1")
        XCTAssertEqual(audio.itemID, "i1")
        let done = try JSONDecoder().decode(ServerEvent.self, from: Data("""
        {"type":"response.done","response":{"id":"r1","status":"completed","output":[]}}
        """.utf8))
        XCTAssertEqual(done.response?.status, "completed")
    }
    func testSessionUsesGAFormatAndDoesNotWaitForTranscription() throws {
        let session = RealtimeProtocol.session(age: .preschool, turnTaking: .responsive, model: "gpt-realtime")
        let json = try RealtimeProtocol.encode(["type": "session.update", "session": session])
        XCTAssertNotNil(try JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any])
        XCTAssertFalse(json.contains("input_audio_format")) // beta schema must not leak in
        let audio = try XCTUnwrap(session["audio"] as? [String: Any])
        let input = try XCTUnwrap(audio["input"] as? [String: Any])
        let format = try XCTUnwrap(input["format"] as? [String: Any])
        XCTAssertEqual(format["type"] as? String, "audio/pcm")
        XCTAssertEqual(format["rate"] as? Int, 24_000)
        let vad = try XCTUnwrap(input["turn_detection"] as? [String: Any])
        XCTAssertEqual(vad["silence_duration_ms"] as? Int, 350)
        XCTAssertEqual(vad["create_response"] as? Bool, true)
        XCTAssertEqual(vad["interrupt_response"] as? Bool, true)
        XCTAssertEqual(TurnTaking.patient.silenceMilliseconds, 650)
    }
}
