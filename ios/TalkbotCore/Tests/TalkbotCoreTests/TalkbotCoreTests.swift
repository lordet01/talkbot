import XCTest
@testable import TalkbotCore

final class TalkbotCoreTests: XCTestCase {
    func testEnglishEllipsisPreservesActivity() {
        var state = ConversationState()
        state.hear("영어놀이 하자")
        let plan = state.hear("강아지는?")
        XCTAssertEqual(state.activity, .english)
        XCTAssertEqual(state.topic, "강아지")
        XCTAssertEqual(plan.beat, .quizWord)
        XCTAssertFalse(plan.allowQuestion)
        XCTAssertTrue(plan.systemPrompt.contains("영어 단어만"))
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
    func testSafetySkipsModelAndReturnsHome() {
        var state = ConversationState()
        state.hear("영어놀이 하자")
        let plan = state.hear("우리 집 주소가 뭐야")
        XCTAssertEqual(state.activity, .free)
        XCTAssertEqual(plan.intent, .safety)
        XCTAssertEqual(plan.canned, "그건 디노랑 안 놀아. 다른 거 하자.")
        XCTAssertTrue(plan.skipLLM)
    }
    func testLockedEnglishIgnoresWeakMealWord() {
        var state = ConversationState()
        state.hear("영어 공부하자")
        state.hear("과자")
        XCTAssertEqual(state.activity, .english)
    }
    func testMealKeepsPickyEating() {
        var state = ConversationState()
        state.hear("배고파")
        let plan = state.hear("브로콜리 싫어")
        XCTAssertEqual(state.activity, .meal)
        XCTAssertEqual(plan.beat, .follow)
        XCTAssertNotEqual(plan.intent, .reject)
    }
    func testYesAcceptsOfferWithoutLeaving() {
        var snap = TurnRouter.Snapshot(activity: .counting, expect: .yn, phase: .offer)
        let plan = TurnRouter.decide(state: &snap, raw: "응", age: .earlySchool)
        XCTAssertEqual(plan.intent, .answer)
        XCTAssertEqual(snap.activity, .counting)
        XCTAssertEqual(snap.phase, .play)
    }
    func testPromptStaysCompactAndCarriesTurnCard() {
        var state = ConversationState()
        let start = state.hear("영어놀이 하자", age: .preschool)
        XCTAssertLessThan(start.systemPrompt.count, 420)
        XCTAssertTrue(start.userPrompt.contains("아이: 영어놀이 하자"))
        let quiz = state.hear("강아지는?")
        XCTAssertTrue(quiz.systemPrompt.contains("이번 턴 질문 금지"))
        XCTAssertTrue(quiz.userPrompt.contains("quizWord"))
    }
    func testRepeatedGarbleUsesCannedListenLine() {
        var state = ConversationState()
        XCTAssertNil(state.hear("zzzz").canned)
        let second = state.hear("zzzz")
        XCTAssertEqual(second.canned, "응, 디노 듣고 있어.")
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
        gaze.tick(now: 10.6)
        XCTAssertTrue(gaze.hasFace)
        gaze.tick(now: 10.71)
        XCTAssertFalse(gaze.hasFace)
        XCTAssertFalse(gaze.lookingAtScreen)
        XCTAssertTrue(gaze.eyesOpen)
        XCTAssertEqual(gaze.point, .center)
    }
    func testGazeUsesPupilOffsetInsideTheFace() {
        let ahead = GazeGeometry.fromLandmarks(face: (0.5, 0.5, 0.3, 0.4), pupil: (0.5, 0.5))
        XCTAssertEqual(ahead.x, 0, accuracy: 0.001)
        XCTAssertEqual(ahead.y, 0, accuracy: 0.001)
        let right = GazeGeometry.fromLandmarks(face: (0.5, 0.5, 0.3, 0.4), pupil: (0.62, 0.5))
        XCTAssertGreaterThan(right.x, 0.3)
        XCTAssertFalse(GazeGeometry.eyesOpen(leftSpan: 0.01, rightSpan: 0.01))
        XCTAssertTrue(GazeGeometry.lookingAtScreen(point: .center, eyesOpen: true, faceWidth: 0.2))
    }
    func testIrisOffsetUsesEyeSocketNotWholeFace() {
        let look = GazeGeometry.irisOffset(pupil: (0.62, 0.55), eye: (midX: 0.5, midY: 0.5, width: 0.2, height: 0.1))
        XCTAssertGreaterThan(look.x, 0.9)
        XCTAssertGreaterThan(look.y, 0.9)
    }
    func testGazeYFollowsUpwardFaceAndIris() {
        let high = GazeGeometry.headPoint(midX: 0.5, midY: 0.7)
        XCTAssertGreaterThan(high.y, 0.3)
        let combined = GazeGeometry.combine(head: GazePoint(x: 0.4, y: 0), look: GazePoint(x: 0.2, y: 0.5))
        XCTAssertGreaterThan(combined.x, 0.3)
        XCTAssertLessThan(combined.x, 0.5)
        XCTAssertGreaterThan(combined.y, 0.2)
    }
    func testARFaceMirrorsLookTowardScreenRightAsNegativeX() {
        let gaze = GazeGeometry.fromARFace(
            faceX: 0, faceY: 0,
            lookOutLeft: 0, lookInLeft: 1,
            lookOutRight: 1, lookInRight: 0,
            lookUpLeft: 0, lookDownLeft: 0,
            lookUpRight: 0, lookDownRight: 0)
        XCTAssertLessThan(gaze.x, -0.3)
    }
    func testBlinkHoldsLastGazeInsteadOfJumping() {
        var gaze = GazeFilter()
        gaze.update(x: 0.6, y: -0.2, eyeContact: false, now: 10, eyesOpen: true)
        let held = gaze.point
        gaze.update(x: 0, y: 0, eyeContact: false, now: 10.05, eyesOpen: false)
        XCTAssertFalse(gaze.eyesOpen)
        XCTAssertEqual(gaze.point.x, held.x, accuracy: 0.0001)
        XCTAssertEqual(gaze.point.y, held.y, accuracy: 0.0001)
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
